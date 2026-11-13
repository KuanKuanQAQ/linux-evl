/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 *
 * Cross-buffer between the oob stage (trusted side, "Z") and the
 * in-band stage (untrusted side, "z"). See
 * docs/22-pipe-final-design.md for the design.
 *
 * Two rings:
 *   ibnd: oob_write()/evl_write_xbuf() -> read()      (Z produces)
 *   obnd: write() -> oob_read()/evl_read_xbuf()       (Z consumes)
 *
 * Each ring is cut into three parts:
 *   - struct xbuf_tend: state of the trusted end, written by trusted
 *     code only, under the lock of its own EVL wait queue.
 *   - struct xbuf_iend: state of the in-band end, written by in-band
 *     code only, under an in-band spinlock. Trusted code never
 *     touches it.
 *   - struct xbuf_shared and bufmem: shared memory. Trusted code
 *     publishes its fill count there, the in-band side may only read
 *     it. Anything the trusted side reads from there is input: the
 *     data bytes, and a "writer waiting" hint.
 *
 * In-band -> trusted: the in-band side reports how many bytes it
 * transferred through xbuf_gate_commit(); the trusted side clamps
 * the count against its own state then updates itself.
 *
 * Trusted -> in-band: the trusted side only records a notification
 * (evl_inband_notify_post()); the in-band side wakes up its own
 * waiters.
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/log2.h>
#include <linux/atomic.h>
#include <evl/thread.h>
#include <evl/xbuf.h>
#include <evl/factory.h>
#include <evl/sched.h>
#include <evl/poll.h>
#include <evl/wait.h>
#include <evl/clock.h>
#include <evl/inband_notify.h>
#include <uapi/evl/xbuf-abi.h>

/* Shared memory: written by one side, read by the other one. */
struct xbuf_shared {
	atomic_t fill;		/* Z -> z: trusted fill count */
	atomic_t oob_waiting;	/* Z -> z: oob callers sleeping */
	atomic_t ib_waiting;	/* z -> Z: in-band callers sleeping, hint */
};

/* Trusted end: trusted code only, under wait.wchan.lock. */
struct xbuf_tend {
	struct evl_wait_queue wait;
	unsigned int fill;	/* bytes readable by the consumer */
	unsigned int off;	/* our own index: W or R */
	unsigned int rsvd;	/* reserved by oob callers, in flight */
	int pending;		/* reservations in flight */
	int waiters;		/* oob callers sleeping on wait */
	struct evl_inband_notify notify;
};

/* In-band end: in-band code only, under lock. */
struct xbuf_iend {
	spinlock_t lock;
	unsigned int off;	/* our own index: W or R */
	unsigned int rsvd;	/* reserved, transfer in flight */
	unsigned int inflight;	/* transferred, being reported */
	int pending;		/* reservations in flight */
	wait_queue_head_t wait;
};

struct xbuf_ring {
	void *bufmem;		/* shared, contents are input */
	unsigned int bufsz;	/* immutable after creation */
	bool oob_produces;
	struct xbuf_tend t;
	struct xbuf_shared *sh;
	struct xbuf_iend *ib;
};

struct evl_xbuf {
	struct evl_element element;
	struct xbuf_ring ibnd;	/* oob_write() -> read() */
	struct xbuf_ring obnd;	/* write() -> oob_read() */
	struct evl_poll_head poll_head;
};

struct xbuf_rdesc {
	char *buf;
	char *buf_ptr;
	size_t count;
	int (*xfer)(struct xbuf_rdesc *dst, char *src, size_t len);
};

static int write_to_user(struct xbuf_rdesc *dst, char *src, size_t len)
{
	return raw_copy_to_user(dst->buf_ptr, src, len);
}

static int write_to_kernel(struct xbuf_rdesc *dst, char *src, size_t len)
{
	memcpy(dst->buf_ptr, src, len);

	return 0;
}

struct xbuf_wdesc {
	const char *buf;
	const char *buf_ptr;
	size_t count;
	int (*xfer)(char *dst, struct xbuf_wdesc *src, size_t len);
};

static int read_from_user(char *dst, struct xbuf_wdesc *src, size_t len)
{
	return raw_copy_from_user(dst, src->buf_ptr, len);
}

static int read_from_kernel(char *dst, struct xbuf_wdesc *src, size_t len)
{
	memcpy(dst, src->buf_ptr, len);

	return 0;
}

/* ================================================= trusted side */

/* Trusted end locked. */
static void tend_publish(struct xbuf_ring *ring)
{
	atomic_set_release(&ring->sh->fill, ring->t.fill);
}

/*
 * Trusted end locked on entry and on return. Sleep until the in-band
 * side reports some progress through the gate. A wake up is not a
 * promise: callers re-evaluate their condition.
 */
static int tend_wait(struct xbuf_ring *ring, unsigned long *flags)
{
	struct xbuf_tend *t = &ring->t;
	int ret;

	t->waiters++;
	atomic_set(&ring->sh->oob_waiting, t->waiters);
	evl_add_wait_queue(&t->wait, EVL_INFINITE, EVL_REL);
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, *flags);
	ret = evl_wait_schedule(&t->wait);
	raw_spin_lock_irqsave(&t->wait.wchan.lock, *flags);
	t->waiters--;
	atomic_set(&ring->sh->oob_waiting, t->waiters);

	return ret;
}

/* Trusted consumer: obnd. */
static ssize_t tend_read(struct xbuf_ring *ring,
			struct xbuf_rdesc *rd, int f_flags)
{
	struct xbuf_tend *t = &ring->t;
	unsigned int len, avail, off, n, rbytes;
	unsigned long flags;
	ssize_t ret;
	int xret;

	if (rd->count == 0)
		return 0;

	if (ring->bufsz == 0)
		return -ENOBUFS;

	len = min_t(size_t, rd->count, UINT_MAX);
	rd->buf_ptr = rd->buf;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);

	for (;;) {
		avail = t->fill - t->rsvd;
		if (avail >= len)
			break;
		if (f_flags & O_NONBLOCK) {
			if (avail == 0) {
				ret = -EAGAIN;
				goto out;
			}
			len = avail;
			break;
		}
		if (len > ring->bufsz) {
			ret = -EINVAL;
			goto out;
		}
		/*
		 * An in-band writer short of space while we are short
		 * of data: allow a short read instead of a deadlock.
		 * The flag is input, any value is harmless.
		 */
		if (avail > 0 && atomic_read(&ring->sh->ib_waiting) > 0) {
			len = avail;
			break;
		}
		ret = tend_wait(ring, &flags);
		if (ret)
			goto out;
	}

	off = t->off;
	t->off = (off + len) % ring->bufsz;
	t->pending++;
	t->rsvd += len;
	rbytes = len;
	ret = len;

	do {
		n = min(rbytes, ring->bufsz - off);
		/* The slot is consumed in any case. */
		raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);
		xret = rd->xfer(rd, ring->bufmem + off, n);
		raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);
		if (xret) {
			ret = -EFAULT;
			break;
		}
		rd->buf_ptr += n;
		rbytes -= n;
		off = (off + n) % ring->bufsz;
	} while (rbytes > 0);

	if (--t->pending == 0) {
		t->fill -= t->rsvd;
		t->rsvd = 0;
		tend_publish(ring);
		/* Room for in-band writers: record only. */
		evl_inband_notify_post(&t->notify);
	}
out:
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	evl_schedule();

	return ret;
}

/* Trusted producer: ibnd. */
static ssize_t tend_write(struct xbuf_ring *ring,
			struct xbuf_wdesc *wd, int f_flags)
{
	struct xbuf_tend *t = &ring->t;
	unsigned int len, off, n, wbytes;
	unsigned long flags;
	ssize_t ret;
	int xret;

	if (wd->count == 0)
		return 0;

	if (ring->bufsz == 0)
		return -ENOBUFS;

	/* No short or scattered writes. */
	if (wd->count > ring->bufsz)
		return -EINVAL;

	len = wd->count;
	wd->buf_ptr = wd->buf;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);

	for (;;) {
		if (t->fill + t->rsvd + len <= ring->bufsz)
			break;
		if (f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		ret = tend_wait(ring, &flags);
		if (ret)
			goto out;
	}

	off = t->off;
	t->off = (off + len) % ring->bufsz;
	t->pending++;
	t->rsvd += len;
	wbytes = len;
	ret = len;

	do {
		n = min(wbytes, ring->bufsz - off);
		raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);
		xret = wd->xfer(ring->bufmem + off, wd, n);
		raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);
		if (xret) {
			/* Cannot roll back, clear what we failed to copy. */
			memset(ring->bufmem + off + n - xret, 0, xret);
			ret = -EFAULT;
			break;
		}
		wd->buf_ptr += n;
		wbytes -= n;
		off = (off + n) % ring->bufsz;
	} while (wbytes > 0);

	if (--t->pending == 0) {
		t->fill += t->rsvd;
		t->rsvd = 0;
		tend_publish(ring);
		/* Data for in-band readers: record only. */
		evl_inband_notify_post(&t->notify);
	}
out:
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	evl_schedule();

	return ret;
}

/*
 * Gate, called by the in-band side: it reports @n bytes transferred
 * on @ring, i.e. consumed from ibnd or produced into obnd. @n is
 * input: it is clamped against the trusted state, which only trusted
 * code updates. A lying peer can only make its own data wrong.
 */
static void xbuf_gate_commit(struct evl_xbuf *xbuf,
			struct xbuf_ring *ring, unsigned int n)
{
	struct xbuf_tend *t = &ring->t;
	unsigned long flags;
	__poll_t events;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);

	if (ring->oob_produces) {
		/* Never past what we published. */
		n = min(n, t->fill);
		t->fill -= n;
		events = POLLOUT|POLLWRNORM;
	} else {
		/* Never over data oob readers have not released. */
		n = min(n, ring->bufsz - t->fill);
		t->fill += n;
		events = POLLIN|POLLRDNORM;
	}

	if (n > 0) {
		tend_publish(ring);
		if (t->waiters > 0)
			evl_flush_wait_locked(&t->wait, 0);
	}

	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	if (n > 0)
		evl_signal_poll_events(&xbuf->poll_head, events);

	evl_schedule();
}

static ssize_t xbuf_oob_read(struct file *filp,
			char __user *u_buf, size_t count)
{
	struct evl_xbuf *xbuf = element_of(filp, struct evl_xbuf);
	struct xbuf_rdesc rd = {
		.buf = u_buf,
		.count = count,
		.xfer = write_to_user,
	};

	return tend_read(&xbuf->obnd, &rd, filp->f_flags);
}

static ssize_t xbuf_oob_write(struct file *filp,
			const char __user *u_buf, size_t count)
{
	struct evl_xbuf *xbuf = element_of(filp, struct evl_xbuf);
	struct xbuf_wdesc wd = {
		.buf = u_buf,
		.count = count,
		.xfer = read_from_user,
	};

	return tend_write(&xbuf->ibnd, &wd, filp->f_flags);
}

static long xbuf_oob_ioctl(struct file *filp,
			unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}

static __poll_t xbuf_oob_poll(struct file *filp,
			struct oob_poll_wait *wait)
{
	struct evl_xbuf *xbuf = element_of(filp, struct evl_xbuf);
	unsigned long flags;
	__poll_t ready = 0;

	evl_poll_watch(&xbuf->poll_head, wait, NULL);

	raw_spin_lock_irqsave(&xbuf->obnd.t.wait.wchan.lock, flags);
	if (xbuf->obnd.t.fill > 0)
		ready |= POLLIN|POLLRDNORM;
	raw_spin_unlock_irqrestore(&xbuf->obnd.t.wait.wchan.lock, flags);

	raw_spin_lock_irqsave(&xbuf->ibnd.t.wait.wchan.lock, flags);
	if (xbuf->ibnd.t.fill < xbuf->ibnd.bufsz)
		ready |= POLLOUT|POLLWRNORM;
	raw_spin_unlock_irqrestore(&xbuf->ibnd.t.wait.wchan.lock, flags);

	return ready;
}

/* =================================================== in-band side */

static unsigned int ib_published(struct xbuf_ring *ring)
{
	return (unsigned int)atomic_read_acquire(&ring->sh->fill);
}

/* Bytes the in-band reader of ibnd may still reserve. */
static unsigned int ib_avail(struct xbuf_ring *ring)
{
	struct xbuf_iend *ib = ring->ib;
	unsigned int fill = ib_published(ring);
	unsigned int busy = READ_ONCE(ib->rsvd) + READ_ONCE(ib->inflight);

	return fill > busy ? fill - busy : 0;
}

/* Bytes the in-band writer of obnd may still reserve. */
static unsigned int ib_room(struct xbuf_ring *ring)
{
	struct xbuf_iend *ib = ring->ib;
	unsigned int used = ib_published(ring) +
		READ_ONCE(ib->rsvd) + READ_ONCE(ib->inflight);

	return ring->bufsz > used ? ring->bufsz - used : 0;
}

/*
 * The last in-flight transfer reports everything reserved so far.
 * Reporting from the last one only keeps the oob side from seeing
 * bytes another in-band transfer is still copying.
 */
static void ib_complete(struct evl_xbuf *xbuf, struct xbuf_ring *ring)
{
	struct xbuf_iend *ib = ring->ib;
	unsigned int done = 0;

	spin_lock(&ib->lock);
	if (--ib->pending == 0) {
		done = ib->rsvd;
		ib->rsvd = 0;
		ib->inflight += done;
	}
	spin_unlock(&ib->lock);

	if (done) {
		xbuf_gate_commit(xbuf, ring, done);
		spin_lock(&ib->lock);
		ib->inflight -= done;
		spin_unlock(&ib->lock);
	}
}

/* In-band consumer: ibnd. */
static ssize_t xbuf_read(struct file *filp, char __user *u_buf,
			size_t count, loff_t *ppos)
{
	struct evl_xbuf *xbuf = element_of(filp, struct evl_xbuf);
	struct xbuf_ring *ring = &xbuf->ibnd;
	struct xbuf_iend *ib = ring->ib;
	unsigned int len, avail, off, n, rbytes;
	ssize_t ret;

	if (count == 0)
		return 0;

	if (ring->bufsz == 0)
		return -ENOBUFS;

	len = min_t(size_t, count, UINT_MAX);

	spin_lock(&ib->lock);

	for (;;) {
		avail = ib_avail(ring);
		if (avail >= len)
			break;
		if (filp->f_flags & O_NONBLOCK) {
			if (avail == 0) {
				spin_unlock(&ib->lock);
				return -EAGAIN;
			}
			len = avail;
			break;
		}
		if (len > ring->bufsz) {
			spin_unlock(&ib->lock);
			return -EINVAL;
		}
		/* An oob writer short of space: short read. */
		if (avail > 0 && atomic_read(&ring->sh->oob_waiting) > 0) {
			len = avail;
			break;
		}
		spin_unlock(&ib->lock);
		ret = wait_event_interruptible(ib->wait, ib_avail(ring) >= len);
		if (ret)
			return ret;
		spin_lock(&ib->lock);
	}

	off = ib->off;
	ib->off = (off + len) % ring->bufsz;
	ib->pending++;
	ib->rsvd += len;
	spin_unlock(&ib->lock);

	rbytes = len;
	ret = len;
	do {
		n = min(rbytes, ring->bufsz - off);
		if (copy_to_user(u_buf, ring->bufmem + off, n)) {
			ret = -EFAULT;
			break;
		}
		u_buf += n;
		rbytes -= n;
		off = (off + n) % ring->bufsz;
	} while (rbytes > 0);

	ib_complete(xbuf, ring);

	return ret;
}

/* In-band producer: obnd. */
static ssize_t xbuf_write(struct file *filp, const char __user *u_buf,
			size_t count, loff_t *ppos)
{
	struct evl_xbuf *xbuf = element_of(filp, struct evl_xbuf);
	struct xbuf_ring *ring = &xbuf->obnd;
	struct xbuf_iend *ib = ring->ib;
	unsigned int len, off, n, wbytes, left;
	ssize_t ret;

	if (count == 0)
		return 0;

	if (ring->bufsz == 0)
		return -ENOBUFS;

	/* No short or scattered writes. */
	if (count > ring->bufsz)
		return -EINVAL;

	len = count;

	spin_lock(&ib->lock);

	while (ib_room(ring) < len) {
		spin_unlock(&ib->lock);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		atomic_inc(&ring->sh->ib_waiting);
		ret = wait_event_interruptible(ib->wait, ib_room(ring) >= len);
		atomic_dec(&ring->sh->ib_waiting);
		if (ret)
			return ret;
		spin_lock(&ib->lock);
	}

	off = ib->off;
	ib->off = (off + len) % ring->bufsz;
	ib->pending++;
	ib->rsvd += len;
	spin_unlock(&ib->lock);

	wbytes = len;
	ret = len;
	do {
		n = min(wbytes, ring->bufsz - off);
		left = copy_from_user(ring->bufmem + off, u_buf, n);
		if (left) {
			memset(ring->bufmem + off + n - left, 0, left);
			ret = -EFAULT;
			break;
		}
		u_buf += n;
		wbytes -= n;
		off = (off + n) % ring->bufsz;
	} while (wbytes > 0);

	ib_complete(xbuf, ring);

	return ret;
}

static long xbuf_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}

static __poll_t xbuf_poll(struct file *filp, poll_table *wait)
{
	struct evl_xbuf *xbuf = element_of(filp, struct evl_xbuf);
	__poll_t ready = 0;

	poll_wait(filp, &xbuf->ibnd.ib->wait, wait);
	poll_wait(filp, &xbuf->obnd.ib->wait, wait);

	if (ib_published(&xbuf->ibnd) > 0)
		ready |= POLLIN|POLLRDNORM;

	if (ib_published(&xbuf->obnd) < xbuf->obnd.bufsz)
		ready |= POLLOUT|POLLWRNORM;

	return ready;
}

static int xbuf_release(struct inode *inode, struct file *filp)
{
	struct evl_xbuf *xbuf = element_of(filp, struct evl_xbuf);

	/* Lifecycle is still driven in-band, see the design notes. */
	evl_flush_wait(&xbuf->obnd.t.wait, EVL_T_RMID);
	evl_flush_wait(&xbuf->ibnd.t.wait, EVL_T_RMID);

	return evl_release_element(inode, filp);
}

static const struct file_operations xbuf_fops = {
	.open		= evl_open_element,
	.release	= xbuf_release,
	.unlocked_ioctl	= xbuf_ioctl,
	.read		= xbuf_read,
	.write		= xbuf_write,
	.poll		= xbuf_poll,
	.oob_ioctl	= xbuf_oob_ioctl,
	.oob_read	= xbuf_oob_read,
	.oob_write	= xbuf_oob_write,
	.oob_poll	= xbuf_oob_poll,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= compat_ptr_ioctl,
	.compat_oob_ioctl  = compat_ptr_oob_ioctl,
#endif
};

/* ============================================== in-kernel interface */

struct evl_xbuf *evl_get_xbuf(int efd, struct evl_file **efilpp)
{
	struct evl_file *efilp = evl_get_file(efd);

	if (efilp && efilp->filp->f_op == &xbuf_fops) {
		*efilpp = efilp;
		return element_of(efilp->filp, struct evl_xbuf);
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(evl_get_xbuf);

void evl_put_xbuf(struct evl_file *efilp)
{
	evl_put_file(efilp);
}
EXPORT_SYMBOL_GPL(evl_put_xbuf);

/*
 * In-kernel callers use the trusted end, so they must run oob. In-band
 * kernel code wanting to talk to the oob side should use the regular
 * read/write file operations instead.
 */
ssize_t evl_read_xbuf(struct evl_xbuf *xbuf, void *buf,
		size_t count, int f_flags)
{
	struct xbuf_rdesc rd = {
		.buf = buf,
		.count = count,
		.xfer = write_to_kernel,
	};

	if (running_inband())
		return -EPERM;

	if (!(f_flags & O_NONBLOCK) && evl_cannot_block())
		return -EPERM;

	return tend_read(&xbuf->obnd, &rd, f_flags);
}
EXPORT_SYMBOL_GPL(evl_read_xbuf);

ssize_t evl_write_xbuf(struct evl_xbuf *xbuf, const void *buf,
		size_t count, int f_flags)
{
	struct xbuf_wdesc wd = {
		.buf = buf,
		.count = count,
		.xfer = read_from_kernel,
	};

	if (running_inband())
		return -EPERM;

	if (!(f_flags & O_NONBLOCK) && evl_cannot_block())
		return -EPERM;

	return tend_write(&xbuf->ibnd, &wd, f_flags);
}
EXPORT_SYMBOL_GPL(evl_write_xbuf);

/* ======================================================== factory */

static int init_ring(struct xbuf_ring *ring, size_t bufsz, bool oob_produces)
{
	ring->bufsz = bufsz;
	ring->oob_produces = oob_produces;

	if (bufsz > 0) {
		ring->bufmem = kzalloc(bufsz, GFP_KERNEL);
		if (ring->bufmem == NULL)
			return -ENOMEM;
	}

	ring->sh = kzalloc(sizeof(*ring->sh), GFP_KERNEL);
	if (ring->sh == NULL)
		goto fail_sh;

	ring->ib = kzalloc(sizeof(*ring->ib), GFP_KERNEL);
	if (ring->ib == NULL)
		goto fail_ib;

	spin_lock_init(&ring->ib->lock);
	init_waitqueue_head(&ring->ib->wait);
	evl_init_wait(&ring->t.wait, &evl_mono_clock, EVL_WAIT_PRIO);
	/* The in-band side waits on ib->wait in both directions. */
	evl_init_inband_notify(&ring->t.notify, EVL_NOTIFY_WAKE_UP, 0,
			&ring->ib->wait);

	return 0;

fail_ib:
	kfree(ring->sh);
fail_sh:
	kfree(ring->bufmem);

	return -ENOMEM;
}

static void destroy_ring(struct xbuf_ring *ring)
{
	evl_destroy_wait(&ring->t.wait);
	evl_inband_notify_drain(&ring->t.notify);
	kfree(ring->ib);
	kfree(ring->sh);
	kfree(ring->bufmem);
}

static struct evl_element *
xbuf_factory_build(struct evl_factory *fac, const char __user *u_name,
		void __user *u_attrs, int clone_flags, u32 *sstate_offp)
{
	struct evl_xbuf_attrs attrs;
	struct evl_xbuf *xbuf;
	int ret;

	ret = copy_from_user(&attrs, u_attrs, sizeof(attrs));
	if (ret)
		return ERR_PTR(-EFAULT);

	if (clone_flags & ~EVL_CLONE_PUBLIC)
		return ERR_PTR(-EINVAL);

	/* LART */
	if ((attrs.i_bufsz == 0 && attrs.o_bufsz == 0) ||
		order_base_2(attrs.i_bufsz) > 30 ||
		order_base_2(attrs.o_bufsz) > 30)
		return ERR_PTR(-EINVAL);

	xbuf = kzalloc(sizeof(*xbuf), GFP_KERNEL);
	if (xbuf == NULL)
		return ERR_PTR(-ENOMEM);

	/* Inbound traffic: oob_write() -> read(). */
	ret = init_ring(&xbuf->ibnd, attrs.i_bufsz, true);
	if (ret)
		goto fail_ibnd;

	/* Outbound traffic: write() -> oob_read(). */
	ret = init_ring(&xbuf->obnd, attrs.o_bufsz, false);
	if (ret)
		goto fail_obnd;

	ret = evl_init_user_element(&xbuf->element, &evl_xbuf_factory,
				u_name, clone_flags);
	if (ret)
		goto fail_element;

	evl_init_poll_head(&xbuf->poll_head);

	return &xbuf->element;

fail_element:
	destroy_ring(&xbuf->obnd);
fail_obnd:
	destroy_ring(&xbuf->ibnd);
fail_ibnd:
	kfree(xbuf);

	return ERR_PTR(ret);
}

static void xbuf_factory_dispose(struct evl_element *e)
{
	struct evl_xbuf *xbuf;

	xbuf = container_of(e, struct evl_xbuf, element);

	destroy_ring(&xbuf->obnd);
	destroy_ring(&xbuf->ibnd);
	evl_destroy_element(&xbuf->element);
	kfree_rcu(xbuf, element.rcu);
}

static ssize_t rings_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct evl_xbuf *xbuf;
	ssize_t ret;

	xbuf = evl_get_element_by_dev(dev, struct evl_xbuf);
	if (xbuf == NULL)
		return -EIO;

	ret = snprintf(buf, PAGE_SIZE, "%u %u %u %u\n",
		READ_ONCE(xbuf->ibnd.t.fill),
		xbuf->ibnd.bufsz,
		READ_ONCE(xbuf->obnd.t.fill),
		xbuf->obnd.bufsz);

	evl_put_element(&xbuf->element);

	return ret;
}
static DEVICE_ATTR_RO(rings);

static struct attribute *xbuf_attrs[] = {
	&dev_attr_rings.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xbuf);

struct evl_factory evl_xbuf_factory = {
	.name	=	EVL_XBUF_DEV,
	.fops	=	&xbuf_fops,
	.build =	xbuf_factory_build,
	.dispose =	xbuf_factory_dispose,
	.nrdev	=	CONFIG_EVL_NR_XBUFS,
	.attrs	=	xbuf_groups,
	.flags	=	EVL_FACTORY_CLONE,
};

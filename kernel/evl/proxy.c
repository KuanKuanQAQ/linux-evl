/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2019 Philippe Gerum  <rpm@xenomai.org>
 *
 * Proxy: lets oob threads write to / read from a regular file through
 * a ring, an in-band relay doing the actual I/O on the target file.
 * See docs/22-pipe-final-design.md for the design.
 *
 *   output: oob_write() -> ring -> relay -> kernel_write(target)
 *   input:  kernel_read(target) -> relay -> ring -> oob_read()
 *
 * Only oob callers use the rings (in-band read()/write() on a proxy
 * are not supported), so each ring has a single trusted end (oob
 * callers, "Z") and a single untrusted end (the relay, "z"):
 *
 *   - struct proxy_tend: trusted end, trusted code only, under the
 *     lock of its own EVL wait queue. It holds the fill count, the
 *     oob index, the oob reservations, the read request (input) and
 *     the relay status (input).
 *   - struct proxy_iend: the relay, in-band code only.
 *   - struct proxy_shared and bufmem: shared. Trusted code publishes
 *     its fill count and read request there; the relay only reads
 *     them. Data bytes are input to the trusted side.
 *
 * Relay -> trusted: proxy_gate_consumed(), proxy_gate_produced() and
 * proxy_gate_status() report what the relay did, as plain numbers
 * which the trusted side clamps.
 *
 * Trusted -> relay: a notification record only (evl_inband_notify),
 * the in-band side queues the relay work itself.
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/log2.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <evl/factory.h>
#include <evl/sched.h>
#include <evl/wait.h>
#include <evl/clock.h>
#include <evl/poll.h>
#include <evl/inband_notify.h>
#include <uapi/evl/proxy-abi.h>

#define EVL_PROXY_CLONE_FLAGS	\
	(EVL_CLONE_PUBLIC|EVL_CLONE_OUTPUT|EVL_CLONE_INPUT)

/* Shared memory: published by the trusted side, read by the relay. */
struct proxy_shared {
	atomic_t fill;		/* trusted fill count */
	atomic_t req;		/* input: bytes the relay may fetch */
};

/* Trusted end: trusted code only, under wait.wchan.lock. */
struct proxy_tend {
	struct evl_wait_queue wait;
	unsigned int fill;	/* bytes readable by the consumer */
	unsigned int off;	/* oob index: W (output) or R (input) */
	unsigned int rsvd;	/* reserved by oob callers, in flight */
	int pending;		/* reservations in flight */
	int waiters;		/* oob callers sleeping on wait */
	unsigned int req;	/* input: outstanding read request */
	int error;		/* input: sticky error from the relay */
	bool eof;		/* input: end of file from the relay */
	struct evl_inband_notify notify; /* kicks the relay */
};

struct evl_proxy;

/* The relay: in-band code only. */
struct proxy_iend {
	struct mutex worker_lock;
	unsigned int off;	/* relay index: R (output) or W (input) */
	struct work_struct work;
	struct workqueue_struct *wq;
	wait_queue_head_t pollq;	/* in-band pollers */
	struct evl_proxy *proxy;
};

struct proxy_ring {
	void *bufmem;		/* shared, contents are input */
	unsigned int bufsz;	/* immutable after creation */
	unsigned int granularity;
	struct proxy_tend t;
	struct proxy_shared *sh;
	struct proxy_iend *ib;
};

struct evl_proxy {
	struct file *filp;	/* target file, used in-band only */
	struct proxy_ring output;
	struct proxy_ring input;
	struct evl_element element;
	struct evl_poll_head poll_head;
};

enum { PROXY_OUTPUT, PROXY_INPUT };

static inline bool proxy_may_read(struct evl_proxy *proxy)
{
	return !!(proxy->element.clone_flags & EVL_CLONE_INPUT);
}

static inline bool proxy_may_write(struct evl_proxy *proxy)
{
	return !!(proxy->element.clone_flags & EVL_CLONE_OUTPUT);
}

/* ================================================= trusted side */

/* Trusted end locked. */
static void tend_publish(struct proxy_ring *ring)
{
	atomic_set_release(&ring->sh->fill, ring->t.fill);
	atomic_set_release(&ring->sh->req, ring->t.req);
}

/*
 * Kick the relay: record only. The pending record holds a reference
 * on the element, which the in-band side hands over to the relay
 * work, so the proxy outlives a running relay.
 */
static void tend_kick_relay(struct evl_proxy *proxy, struct proxy_ring *ring)
{
	evl_get_element(&proxy->element);
	if (!evl_inband_notify_post(&ring->t.notify))
		evl_put_element(&proxy->element);
}

/* Trusted end locked on entry and on return. */
static int tend_wait(struct proxy_ring *ring, unsigned long *flags)
{
	struct proxy_tend *t = &ring->t;
	int ret;

	t->waiters++;
	evl_add_wait_queue(&t->wait, EVL_INFINITE, EVL_REL);
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, *flags);
	ret = evl_wait_schedule(&t->wait);
	raw_spin_lock_irqsave(&t->wait.wchan.lock, *flags);
	t->waiters--;

	return ret;
}

/* Trusted end locked. Never ask for more than the ring can hold. */
static void tend_request(struct evl_proxy *proxy, struct proxy_ring *ring,
			unsigned int len)
{
	struct proxy_tend *t = &ring->t;
	unsigned int room = ring->bufsz - t->fill;

	t->req = min(t->req + len, room);
	tend_publish(ring);
	tend_kick_relay(proxy, ring);
}

static ssize_t proxy_oob_write(struct file *filp,
			const char __user *u_buf, size_t count)
{
	struct evl_proxy *proxy = element_of(filp, struct evl_proxy);
	struct proxy_ring *ring = &proxy->output;
	struct proxy_tend *t = &ring->t;
	unsigned int len, off, n, wbytes;
	unsigned long flags;
	ssize_t ret;
	int xret;

	if (!proxy_may_write(proxy))
		return -ENXIO;

	if (count == 0)
		return 0;

	if (count > ring->bufsz)
		return -EFBIG;

	if (ring->granularity > 1 && count % ring->granularity > 0)
		return -EINVAL;

	len = count;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);

	/* No short or scattered writes. */
	while (t->fill + t->rsvd + len > ring->bufsz) {
		if (filp->f_flags & O_NONBLOCK) {
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
		xret = raw_copy_from_user(ring->bufmem + off, u_buf, n);
		raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);
		if (xret) {
			memset(ring->bufmem + off + n - xret, 0, xret);
			ret = -EFAULT;
			break;
		}
		u_buf += n;
		wbytes -= n;
		off = (off + n) % ring->bufsz;
	} while (wbytes > 0);

	if (--t->pending == 0) {
		t->fill += t->rsvd;
		t->rsvd = 0;
		tend_publish(ring);
		tend_kick_relay(proxy, ring);
	}
out:
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	evl_schedule();

	return ret == -EIDRM ? -EBADF : ret;
}

static ssize_t proxy_oob_read(struct file *filp,
			char __user *u_buf, size_t count)
{
	struct evl_proxy *proxy = element_of(filp, struct evl_proxy);
	struct proxy_ring *ring = &proxy->input;
	struct proxy_tend *t = &ring->t;
	unsigned int len, avail, off, n, rbytes;
	bool requested = false;
	unsigned long flags;
	ssize_t ret;
	int xret;

	if (!proxy_may_read(proxy))
		return -ENXIO;

	if (count == 0)
		return 0;

	if (count > ring->bufsz)
		return -EFBIG;

	if (ring->granularity > 1 && count % ring->granularity > 0)
		return -EINVAL;

	len = count;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);

	for (;;) {
		avail = t->fill - t->rsvd;
		if (avail >= len)
			break;
		if (avail > 0 && (filp->f_flags & O_NONBLOCK)) {
			/* granularity may not be ^2. */
			n = rounddown(avail, ring->granularity ?: 1);
			if (n) {
				len = n;
				break;
			}
		}
		if (t->error) {
			ret = t->error;
			goto out;
		}
		/* Non-blocking callers get 0 when nothing is there. */
		if (filp->f_flags & O_NONBLOCK) {
			ret = 0;
			goto out;
		}
		if (!requested) {
			tend_request(proxy, ring, len);
			requested = true;
		}
		ret = tend_wait(ring, &flags);
		if (ret)
			goto out;
		if (t->eof) {
			t->eof = false;
			ret = 0;
			goto out;
		}
	}

	off = t->off;
	t->off = (off + len) % ring->bufsz;
	t->pending++;
	t->rsvd += len;
	rbytes = len;
	ret = len;

	do {
		n = min(rbytes, ring->bufsz - off);
		raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);
		xret = raw_copy_to_user(u_buf, ring->bufmem + off, n);
		raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);
		if (xret) {
			ret = -EFAULT;
			break;
		}
		u_buf += n;
		rbytes -= n;
		off = (off + n) % ring->bufsz;
	} while (rbytes > 0);

	if (--t->pending == 0) {
		t->fill -= t->rsvd;
		t->rsvd = 0;
		tend_publish(ring);
	}
out:
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	evl_schedule();

	return ret == -EIDRM ? -EBADF : ret;
}

static __poll_t proxy_oob_poll(struct file *filp,
			struct oob_poll_wait *wait)
{
	struct evl_proxy *proxy = element_of(filp, struct evl_proxy);
	struct proxy_ring *oring = &proxy->output;
	struct proxy_ring *iring = &proxy->input;
	unsigned long flags;
	__poll_t ret = 0;

	evl_poll_watch(&proxy->poll_head, wait, NULL);

	if (proxy_may_write(proxy)) {
		raw_spin_lock_irqsave(&oring->t.wait.wchan.lock, flags);
		if (oring->t.fill < oring->bufsz)
			ret |= POLLOUT|POLLWRNORM;
		raw_spin_unlock_irqrestore(&oring->t.wait.wchan.lock, flags);
	}

	/*
	 * If the input ring is empty, ask the relay for a readahead.
	 * The events the target file might have pending itself are
	 * not collected: that would mean calling into it.
	 */
	if (proxy_may_read(proxy)) {
		raw_spin_lock_irqsave(&iring->t.wait.wchan.lock, flags);
		if (iring->t.fill > 0)
			ret |= POLLIN|POLLRDNORM;
		else if (iring->t.req == 0)
			tend_request(proxy, iring, iring->granularity ?: 1);
		raw_spin_unlock_irqrestore(&iring->t.wait.wchan.lock, flags);
	}

	return ret;
}

/*
 * Gate: the relay sent @n bytes out of the output ring. Input,
 * clamped to what we published.
 */
static void proxy_gate_consumed(struct evl_proxy *proxy, unsigned int n)
{
	struct proxy_ring *ring = &proxy->output;
	struct proxy_tend *t = &ring->t;
	unsigned long flags;
	bool drained;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);
	n = min(n, t->fill);
	t->fill -= n;
	drained = t->fill == 0;
	if (n > 0) {
		tend_publish(ring);
		if (t->waiters > 0)
			evl_flush_wait_locked(&t->wait, 0);
	}
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	/* For proxies, writability means all pending data was sent. */
	if (drained)
		evl_signal_poll_events(&proxy->poll_head, POLLOUT|POLLWRNORM);

	evl_schedule();
}

/*
 * Gate: the relay fetched @n bytes into the input ring. Input,
 * clamped to the room left in the ring.
 */
static void proxy_gate_produced(struct evl_proxy *proxy, unsigned int n)
{
	struct proxy_ring *ring = &proxy->input;
	struct proxy_tend *t = &ring->t;
	unsigned long flags;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);
	n = min(n, ring->bufsz - t->fill);
	t->fill += n;
	t->req -= min(n, t->req);
	if (n > 0) {
		tend_publish(ring);
		if (t->waiters > 0)
			evl_flush_wait_locked(&t->wait, 0);
	}
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	if (n > 0)
		evl_signal_poll_events(&proxy->poll_head, POLLIN|POLLRDNORM);

	evl_schedule();
}

/*
 * Gate: the relay hit end of file (@status == 0) or an error. Input:
 * anything which is not a valid error code becomes -EIO.
 */
static void proxy_gate_status(struct evl_proxy *proxy, int status)
{
	struct proxy_ring *ring = &proxy->input;
	struct proxy_tend *t = &ring->t;
	unsigned long flags;

	raw_spin_lock_irqsave(&t->wait.wchan.lock, flags);
	if (status == 0)
		t->eof = true;
	else if (status < 0 && status >= -MAX_ERRNO)
		t->error = status;
	else
		t->error = -EIO;
	t->req = 0;
	tend_publish(ring);
	if (t->waiters > 0)
		evl_flush_wait_locked(&t->wait, 0);
	raw_spin_unlock_irqrestore(&t->wait.wchan.lock, flags);

	evl_signal_poll_events(&proxy->poll_head, POLLIN|POLLRDNORM);

	evl_schedule();
}

/* ============================================ in-band side: relay */

static void relay_output(struct evl_proxy *proxy)
{
	struct proxy_ring *ring = &proxy->output;
	struct proxy_iend *ib = ring->ib;
	struct file *filp = proxy->filp;
	unsigned int count, len, n, off;
	loff_t pos, *ppos;
	ssize_t ret = 0;

	mutex_lock(&ib->worker_lock);

	ppos = NULL;
	if (filp->f_mode & FMODE_ATOMIC_POS) {
		mutex_lock(&filp->f_pos_lock);
		ppos = &pos;
		pos = filp->f_pos;
	}

	while (ret >= 0) {
		count = min_t(unsigned int,
			atomic_read_acquire(&ring->sh->fill), ring->bufsz);
		if (count == 0)
			break;
		len = count;
		off = ib->off;
		do {
			n = min(len, ring->bufsz - off);
			if (ring->granularity > 0)
				n = min(n, ring->granularity);
			ret = kernel_write(filp, ring->bufmem + off, n, ppos);
			if (ret >= 0 && ppos)
				filp->f_pos = *ppos;
			len -= n;
			off = (off + n) % ring->bufsz;
		} while (len > 0 && ret > 0);
		/*
		 * On error, the portion we failed writing is
		 * lost. Fair enough. Skip it so that our index stays in
		 * line with the count we report.
		 */
		ib->off = (ib->off + count) % ring->bufsz;
		proxy_gate_consumed(proxy, count);
		wake_up(&ib->pollq);
	}

	if (ppos)
		mutex_unlock(&filp->f_pos_lock);

	mutex_unlock(&ib->worker_lock);
}

static void relay_input(struct evl_proxy *proxy)
{
	struct proxy_ring *ring = &proxy->input;
	struct proxy_iend *ib = ring->ib;
	struct file *filp = proxy->filp;
	unsigned int count, n;
	loff_t pos, *ppos;
	ssize_t ret;

	mutex_lock(&ib->worker_lock);

	ppos = NULL;
	if (filp->f_mode & FMODE_ATOMIC_POS) {
		mutex_lock(&filp->f_pos_lock);
		ppos = &pos;
		pos = filp->f_pos;
	}

	for (;;) {
		/* The trusted side keeps req within the free room. */
		count = min_t(unsigned int,
			atomic_read_acquire(&ring->sh->req), ring->bufsz);
		if (count == 0)
			break;
		n = min(count, ring->bufsz - ib->off);
		if (ring->granularity > 0)
			n = min(n, ring->granularity);
		ret = kernel_read(filp, ring->bufmem + ib->off, n, ppos);
		if (ret <= 0) {
			proxy_gate_status(proxy, ret);
			wake_up(&ib->pollq);
			break;
		}
		if (ppos)
			filp->f_pos = *ppos;
		ib->off = (ib->off + ret) % ring->bufsz;
		proxy_gate_produced(proxy, ret);
		wake_up(&ib->pollq);
	}

	if (ppos)
		mutex_unlock(&filp->f_pos_lock);

	mutex_unlock(&ib->worker_lock);
}

static void relay_work(struct work_struct *work)
{
	struct proxy_iend *ib = container_of(work, struct proxy_iend, work);
	struct evl_proxy *proxy = ib->proxy;

	if (ib == proxy->output.ib)
		relay_output(proxy);
	else
		relay_input(proxy);

	/* Drop the reference the trusted side took when kicking us. */
	evl_put_element(&proxy->element);
}

/*
 * Called by the in-band handler of the notification relay, in
 * in-band IRQ context. The cookie is the relay to kick.
 */
static void relay_kick(const struct evl_notify_event *ev)
{
	struct proxy_iend *ib = ev->cookie;

	/* Already queued: that work holds its own reference. */
	if (!queue_work(ib->wq, &ib->work))
		evl_put_element(&ib->proxy->element);
}

/* =================================================== file operations */

/*
 * In-band read()/write() on a proxy are not supported: only oob
 * threads use the rings. In-band callers should use the target file
 * directly, EVL threads may use oob_read()/oob_write().
 */
static ssize_t proxy_write(struct file *filp, const char __user *u_buf,
			size_t count, loff_t *ppos)
{
	return -EOPNOTSUPP;
}

static ssize_t proxy_read(struct file *filp,
			char __user *u_buf, size_t count, loff_t *ppos)
{
	return -EOPNOTSUPP;
}

/*
 * In-band poll: in-band side only. It reads what the trusted side
 * published and forwards to the target file, which is in-band code
 * calling in-band code. Ring readiness only ever improves through the
 * relay, which wakes pollq after reporting.
 */
static __poll_t proxy_poll(struct file *filp, poll_table *wait)
{
	struct evl_proxy *proxy = element_of(filp, struct evl_proxy);
	struct proxy_ring *oring = &proxy->output;
	struct proxy_ring *iring = &proxy->input;
	__poll_t ret = 0;

	if (proxy_may_write(proxy)) {
		poll_wait(filp, &oring->ib->pollq, wait);
		if ((unsigned int)atomic_read_acquire(&oring->sh->fill) < oring->bufsz)
			ret = POLLOUT|POLLWRNORM;
	}

	if (proxy_may_read(proxy)) {
		poll_wait(filp, &iring->ib->pollq, wait);
		if (atomic_read_acquire(&iring->sh->fill) > 0)
			ret |= POLLIN|POLLRDNORM;
	}

	if (proxy->filp->f_op->poll)
		ret |= proxy->filp->f_op->poll(proxy->filp, wait);

	return ret;
}

static int proxy_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct evl_proxy *proxy = element_of(filp, struct evl_proxy);
	struct file *mapfilp = proxy->filp;
	int ret;

	if (mapfilp->f_op->mmap == NULL)
		return -ENODEV;

	vma->vm_file = get_file(mapfilp);

	/*
	 * Since the mapper element impersonates a different file, we
	 * need to swap references: if the mapping call fails, we have
	 * to drop the reference on the target file we just took on
	 * entry; if it succeeds, then we have to drop the reference
	 * on the mapper file do_mmap_pgoff() acquired before calling
	 * us.
	 */
	ret = call_mmap(mapfilp, vma);
	if (ret)
		fput(mapfilp);
	else
		fput(filp);

	return ret;
}

static int proxy_release(struct inode *inode, struct file *filp)
{
	struct evl_proxy *proxy = element_of(filp, struct evl_proxy);

	/* Lifecycle is still driven in-band, see the design notes. */
	if (proxy_may_write(proxy))
		evl_flush_wait(&proxy->output.t.wait, EVL_T_RMID);

	if (proxy_may_read(proxy))
		evl_flush_wait(&proxy->input.t.wait, EVL_T_RMID);

	return evl_release_element(inode, filp);
}

static const struct file_operations proxy_fops = {
	.open		= evl_open_element,
	.release	= proxy_release,
	.oob_write	= proxy_oob_write,
	.oob_read	= proxy_oob_read,
	.oob_poll	= proxy_oob_poll,
	.write		= proxy_write,
	.read		= proxy_read,
	.poll		= proxy_poll,
	.mmap		= proxy_mmap,
};

/* ======================================================== factory */

static int init_ring(struct proxy_ring *ring,
		struct evl_proxy *proxy,
		size_t bufsz,
		unsigned int granularity,
		bool is_output)
{
	struct proxy_iend *ib;

	ring->bufsz = bufsz;
	ring->granularity = granularity;

	ring->bufmem = kzalloc(bufsz, GFP_KERNEL);
	if (ring->bufmem == NULL)
		return -ENOMEM;

	ring->sh = kzalloc(sizeof(*ring->sh), GFP_KERNEL);
	if (ring->sh == NULL)
		goto fail_sh;

	ib = kzalloc(sizeof(*ib), GFP_KERNEL);
	if (ib == NULL)
		goto fail_ib;

	ib->wq = alloc_ordered_workqueue("%s.%c", 0,
				evl_element_name(&proxy->element),
				is_output ? 'O' : 'I');
	if (ib->wq == NULL)
		goto fail_wq;

	mutex_init(&ib->worker_lock);
	init_waitqueue_head(&ib->pollq);
	INIT_WORK(&ib->work, relay_work);
	ib->proxy = proxy;
	ring->ib = ib;

	evl_init_wait(&ring->t.wait, &evl_mono_clock, EVL_WAIT_PRIO);
	evl_init_inband_call(&ring->t.notify, relay_kick, ib,
			is_output ? PROXY_OUTPUT : PROXY_INPUT);

	return 0;

fail_wq:
	kfree(ib);
fail_ib:
	kfree(ring->sh);
fail_sh:
	kfree(ring->bufmem);

	return -ENOMEM;
}

static void destroy_ring(struct proxy_ring *ring)
{
	/*
	 * Every pending record and every queued or running relay
	 * holds a reference on the element, so none is left once we
	 * get there. Draining and cancelling are only safety nets;
	 * disposal never runs from a relay work (see
	 * __evl_put_element()).
	 */
	evl_inband_notify_drain(&ring->t.notify);
	cancel_work_sync(&ring->ib->work);
	destroy_workqueue(ring->ib->wq);
	evl_destroy_wait(&ring->t.wait);
	kfree(ring->ib);
	kfree(ring->sh);
	kfree(ring->bufmem);
}

static struct evl_element *
proxy_factory_build(struct evl_factory *fac, const char __user *u_name,
		void __user *u_attrs, int clone_flags, u32 *sstate_offp)
{
	struct evl_proxy_attrs attrs;
	struct evl_proxy *proxy;
	struct file *filp;
	size_t bufsz;
	int ret;

	if (clone_flags & ~EVL_PROXY_CLONE_FLAGS)
		return ERR_PTR(-EINVAL);

	ret = copy_from_user(&attrs, u_attrs, sizeof(attrs));
	if (ret)
		return ERR_PTR(-EFAULT);

	bufsz = attrs.bufsz;
	if (order_base_2(bufsz) > 30) /* LART */
		return ERR_PTR(-EINVAL);

	if (bufsz == 0 && (clone_flags & (EVL_CLONE_INPUT|EVL_CLONE_OUTPUT)))
		return ERR_PTR(-EINVAL);

	/*
	 * If a granularity is set, the buffer size must be a multiple
	 * of the granule size.
	 */
	if (attrs.granularity > 1 && bufsz % attrs.granularity > 0)
		return ERR_PTR(-EINVAL);

	filp = fget(attrs.fd);
	if (filp == NULL)
		return ERR_PTR(-EBADF);

	proxy = kzalloc(sizeof(*proxy), GFP_KERNEL);
	if (proxy == NULL) {
		ret = -ENOMEM;
		goto fail_proxy;
	}

	/*
	 * No direction specified but a valid buffer size implies
	 * EVL_CLONE_OUTPUT.
	 */
	if (bufsz > 0 && !(clone_flags & (EVL_CLONE_INPUT|EVL_CLONE_OUTPUT)))
		clone_flags |= EVL_CLONE_OUTPUT;

	ret = evl_init_user_element(&proxy->element,
				&evl_proxy_factory, u_name, clone_flags);
	if (ret)
		goto fail_element;

	proxy->filp = filp;
	evl_init_poll_head(&proxy->poll_head);

	if (clone_flags & EVL_CLONE_OUTPUT) {
		ret = init_ring(&proxy->output, proxy, bufsz,
				attrs.granularity, true);
		if (ret)
			goto fail_output_init;
	}

	if (clone_flags & EVL_CLONE_INPUT) {
		ret = init_ring(&proxy->input, proxy, bufsz,
				attrs.granularity, false);
		if (ret)
			goto fail_input_init;
	}

	return &proxy->element;

fail_input_init:
	if (clone_flags & EVL_CLONE_OUTPUT)
		destroy_ring(&proxy->output);
fail_output_init:
	evl_destroy_element(&proxy->element);
fail_element:
	kfree(proxy);
fail_proxy:
	fput(filp);

	return ERR_PTR(ret);
}

static void proxy_factory_dispose(struct evl_element *e)
{
	struct evl_proxy *proxy;

	proxy = container_of(e, struct evl_proxy, element);

	if (proxy_may_write(proxy))
		destroy_ring(&proxy->output);

	if (proxy_may_read(proxy))
		destroy_ring(&proxy->input);

	fput(proxy->filp);

	evl_destroy_element(&proxy->element);

	kfree_rcu(proxy, element.rcu);
}

struct evl_factory evl_proxy_factory = {
	.name	=	EVL_PROXY_DEV,
	.fops	=	&proxy_fops,
	.build =	proxy_factory_build,
	.dispose =	proxy_factory_dispose,
	.nrdev	=	CONFIG_EVL_NR_PROXIES,
	.flags	=	EVL_FACTORY_CLONE,
};

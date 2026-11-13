/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Trusted -> untrusted notification relay ("record, do not call").
 * See docs/22-pipe-final-design.md and docs/24-core-cut.md.
 *
 * Trusted code never calls in-band code nor writes in-band data
 * structures to notify the in-band side. It only marks a notifier
 * which lives in trusted memory as pending, links it to a per-CPU list
 * which also lives in trusted memory, then posts a synthetic IRQ to
 * the in-band stage. The in-band handler of that IRQ pulls pending
 * entries through a gate, receiving a copy of the record (cookie, tag,
 * accumulated bits, last value), and performs the in-band action
 * itself (wake_up(), complete(), wake_up_process(), queue_work(),
 * send_sig()...).
 *
 * Delivery is not guaranteed to be exact: several posts may coalesce
 * into a single delivery, and the in-band side may delay or skip a
 * delivery altogether. Receivers must re-evaluate their wait
 * condition, as with any wake up. A missing delivery only hurts the
 * in-band side (liveness), never trusted state.
 *
 * This replaces every irq_work the EVL core used to queue from the
 * oob stage: the irq_work list and the irq_work flags belong to the
 * in-band kernel, irq_work_queue() is in-band code.
 */

#ifndef _EVL_INBAND_NOTIFY_H
#define _EVL_INBAND_NOTIFY_H

#include <linux/llist.h>
#include <linux/bitops.h>
#include <linux/atomic.h>

enum evl_inband_notify_type {
	/* cookie is a struct wait_queue_head *: wake_up() it. */
	EVL_NOTIFY_WAKE_UP,
	/* cookie is a struct wait_queue_head *: wake_up_all() it. */
	EVL_NOTIFY_WAKE_ALL,
	/* cookie is a struct task_struct *: wake_up_process() it. */
	EVL_NOTIFY_WAKE_TASK,
	/* cookie is a struct completion *: complete() it. */
	EVL_NOTIFY_COMPLETE,
	/* call ->fn(&event) from the in-band handler. */
	EVL_NOTIFY_CALL,
};

/* Bits in evl_inband_notify.flags. */
#define EVL_NOTIFY_PENDING	0	/* queued, not taken yet */
#define EVL_NOTIFY_BUSY		1	/* taken, being delivered */
#define EVL_NOTIFY_ONESHOT	2	/* receiver may release the record */

/* What the in-band side receives: a copy, never the record. */
struct evl_notify_event {
	unsigned int type;
	unsigned int tag;
	void *cookie;
	unsigned long bits;	/* bits posted since the last delivery */
	unsigned long value;	/* last value posted */
};

typedef void (*evl_notify_fn_t)(const struct evl_notify_event *ev);

struct evl_inband_notify {
	struct llist_node node;
	unsigned long flags;
	unsigned int type;
	unsigned int tag;
	void *cookie;		/* in-band object, never dereferenced here */
	evl_notify_fn_t fn;	/* EVL_NOTIFY_CALL */
	atomic_long_t bits;
	unsigned long value;
};

#define EVL_INBAND_NOTIFY_INIT(__type, __tag, __cookie, __fn)	\
	{							\
		.flags = 0,					\
		.type = (__type),				\
		.tag = (__tag),					\
		.cookie = (__cookie),				\
		.fn = (__fn),					\
		.bits = ATOMIC_LONG_INIT(0),			\
		.value = 0,					\
	}

static inline void evl_init_inband_notify(struct evl_inband_notify *n,
					unsigned int type, unsigned int tag,
					void *cookie)
{
	n->flags = 0;
	n->type = type;
	n->tag = tag;
	n->cookie = cookie;
	n->fn = NULL;
	atomic_long_set(&n->bits, 0);
	n->value = 0;
}

static inline void evl_init_inband_call(struct evl_inband_notify *n,
					evl_notify_fn_t fn, void *cookie,
					unsigned int tag)
{
	evl_init_inband_notify(n, EVL_NOTIFY_CALL, tag, cookie);
	n->fn = fn;
}

/*
 * A one-shot record is posted once in its lifetime, and its receiver
 * may release the memory holding it: the relay never touches it after
 * calling the receiver, and evl_inband_notify_drain() must not be used
 * on it.
 */
static inline void evl_set_inband_notify_oneshot(struct evl_inband_notify *n)
{
	__set_bit(EVL_NOTIFY_ONESHOT, &n->flags);
}

/*
 * Any stage, any context. Never calls in-band code. Returns true if
 * @n was queued, false if it was already pending. When called from
 * the in-band stage with interrupts enabled, the delivery happens
 * before returning.
 */
bool evl_inband_notify_post(struct evl_inband_notify *n);

/*
 * Same as evl_inband_notify_post(), accumulating @bits before
 * posting. The receiver gets the union of all bits posted since its
 * last delivery.
 */
bool evl_inband_notify_post_bits(struct evl_inband_notify *n,
				unsigned long bits);

/*
 * Replace the value the receiver gets with its next delivery. Call
 * before posting.
 */
static inline void evl_inband_notify_set_value(struct evl_inband_notify *n,
					unsigned long value)
{
	WRITE_ONCE(n->value, value);
}

/*
 * In-band, task context, once no further post may happen on @n:
 * wait for any pending or ongoing delivery of @n to complete, so that
 * @n and its cookie may be released.
 */
void evl_inband_notify_drain(struct evl_inband_notify *n);

static inline bool evl_inband_notify_idle(struct evl_inband_notify *n)
{
	return !(READ_ONCE(n->flags) &
		(BIT(EVL_NOTIFY_PENDING) | BIT(EVL_NOTIFY_BUSY)));
}

int evl_inband_notify_init(void);

void evl_inband_notify_cleanup(void);

#endif /* !_EVL_INBAND_NOTIFY_H */

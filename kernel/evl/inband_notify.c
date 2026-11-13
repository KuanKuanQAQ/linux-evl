/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Trusted -> untrusted notification relay, see
 * include/evl/inband_notify.h.
 */

#include <linux/percpu.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irq_pipeline.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <evl/inband_notify.h>

/* ------------------------------------------------ trusted side */

static DEFINE_PER_CPU(struct llist_head, evl_notify_list);

static unsigned int evl_notify_sirq;

bool evl_inband_notify_post(struct evl_inband_notify *n)
{
	unsigned long flags;

	if (test_and_set_bit(EVL_NOTIFY_PENDING, &n->flags))
		return false;

	/*
	 * The entry must land on the list of the CPU which receives
	 * the synthetic IRQ, so do not migrate in between.
	 */
	flags = hard_local_irq_save();
	llist_add(&n->node, this_cpu_ptr(&evl_notify_list));
	irq_post_inband(evl_notify_sirq);
	/*
	 * Posting from the in-band stage: the log is only synced on
	 * the next interrupt or unstall, so do it now if we may, like
	 * irq_local_work_raise() does for irq_work.
	 */
	if (running_inband() &&
	    !hard_irqs_disabled_flags(flags) && !irqs_disabled())
		sync_current_irq_stage();
	hard_local_irq_restore(flags);

	return true;
}
EXPORT_SYMBOL_GPL(evl_inband_notify_post);

bool evl_inband_notify_post_bits(struct evl_inband_notify *n,
				unsigned long bits)
{
	atomic_long_or(bits, &n->bits);
	/* Bits visible before PENDING is tested, see the take gate. */
	smp_mb__after_atomic();

	return evl_inband_notify_post(n);
}
EXPORT_SYMBOL_GPL(evl_inband_notify_post_bits);

/*
 * What the take gate hands over to the in-band side: a copy of the
 * record's values, nothing which points back into the record. The
 * done gate finds the record on its own (evl_notify_taken).
 */
struct evl_notify_take {
	evl_notify_fn_t fn;
	struct evl_notify_event ev;
};

/*
 * The record being delivered on this CPU, NULL if none or one-shot.
 * Set by the take gate, cleared by the done gate. The in-band handler
 * runs in (in-band) interrupt context and does not nest on a CPU, so
 * there is at most one such record per CPU.
 */
static DEFINE_PER_CPU(struct evl_inband_notify *, evl_notify_taken);

/*
 * Gate: pull one pending notification from the current CPU's list
 * and hand over a copy of it. Only the in-band handler running on
 * this CPU consumes this list, which is what llist_del_first()
 * requires; producers may run concurrently from the oob stage.
 *
 * The entry is marked busy before it stops being pending, so that
 * evl_inband_notify_drain() never sees a gap. The pending bit is
 * cleared before the accumulated bits are collected: a post racing
 * with the delivery either sees PENDING set and its bits are
 * collected here, or queues the entry again.
 */
static bool evl_gate_notify_take(struct evl_notify_take *t)
{
	struct evl_inband_notify *n;
	struct llist_node *node;
	unsigned long flags;
	bool oneshot;

	flags = hard_local_irq_save();
	node = llist_del_first(this_cpu_ptr(&evl_notify_list));
	hard_local_irq_restore(flags);
	if (node == NULL)
		return false;

	n = container_of(node, struct evl_inband_notify, node);
	oneshot = test_bit(EVL_NOTIFY_ONESHOT, &n->flags);
	t->ev.type = n->type;
	t->ev.tag = n->tag;
	t->ev.cookie = n->cookie;
	t->fn = READ_ONCE(n->fn);
	if (!oneshot)
		set_bit(EVL_NOTIFY_BUSY, &n->flags);
	/*
	 * A one-shot record may be released by its receiver, so we
	 * must not remember it past this point.
	 */
	this_cpu_write(evl_notify_taken, oneshot ? NULL : n);
	clear_bit_unlock(EVL_NOTIFY_PENDING, &n->flags);
	smp_mb__after_atomic();
	t->ev.bits = atomic_long_xchg(&n->bits, 0);
	t->ev.value = READ_ONCE(n->value);

	return true;
}

/* Gate: the in-band action for the last taken record is done. */
static void evl_gate_notify_done(void)
{
	struct evl_inband_notify *n = this_cpu_read(evl_notify_taken);

	if (n) {
		this_cpu_write(evl_notify_taken, NULL);
		clear_bit_unlock(EVL_NOTIFY_BUSY, &n->flags);
	}
}

/* ------------------------------------------------ in-band side */

static irqreturn_t evl_notify_handler(int sirq, void *dev_id)
{
	struct evl_notify_take t;

	while (evl_gate_notify_take(&t)) {
		switch (t.ev.type) {
		case EVL_NOTIFY_WAKE_UP:
			wake_up(t.ev.cookie);
			break;
		case EVL_NOTIFY_WAKE_ALL:
			wake_up_all(t.ev.cookie);
			break;
		case EVL_NOTIFY_WAKE_TASK:
			wake_up_process(t.ev.cookie);
			break;
		case EVL_NOTIFY_COMPLETE:
			complete(t.ev.cookie);
			break;
		case EVL_NOTIFY_CALL:
			/*
			 * fn was copied by the take gate: a one-shot
			 * receiver may release the record in there.
			 */
			t.fn(&t.ev);
			break;
		}
		evl_gate_notify_done();
	}

	return IRQ_HANDLED;
}

void evl_inband_notify_drain(struct evl_inband_notify *n)
{
	might_sleep();

	while (!evl_inband_notify_idle(n))
		schedule_timeout_uninterruptible(1);
}
EXPORT_SYMBOL_GPL(evl_inband_notify_drain);

static DEFINE_PER_CPU(int, evl_notify_dev);

int __init evl_inband_notify_init(void)
{
	int sirq, ret, cpu;

	for_each_possible_cpu(cpu)
		init_llist_head(per_cpu_ptr(&evl_notify_list, cpu));

	sirq = irq_create_direct_mapping(synthetic_irq_domain);
	if (sirq == 0)
		return -EAGAIN;

	ret = __request_percpu_irq(sirq, evl_notify_handler,
				IRQF_NO_THREAD, "EVL in-band notify",
				&evl_notify_dev);
	if (ret) {
		irq_dispose_mapping(sirq);
		return ret;
	}

	evl_notify_sirq = sirq;

	return 0;
}

void evl_inband_notify_cleanup(void)
{
	free_percpu_irq(evl_notify_sirq, &evl_notify_dev);
	irq_dispose_mapping(evl_notify_sirq);
}

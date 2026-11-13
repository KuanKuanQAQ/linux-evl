/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_MONITOR_H
#define _EVL_MONITOR_H

#include <linux/list.h>
#include <linux/wait.h>
#include <evl/wait.h>
#include <evl/thread.h>
#include <evl/mutex.h>
#include <evl/poll.h>
#include <evl/factory.h>
#include <evl/inband_notify.h>
#include <uapi/evl/monitor-abi.h>

struct evl_monitor_ib;

struct evl_monitor {
	struct evl_element element;
	struct __evl_monitor_sstate *sstate;
	int type : 2,
	    protocol : 4;
	union {
		struct {
			struct evl_mutex mutex;
			struct list_head events;
			hard_spinlock_t lock;
		};
		struct {
			/* Out-of-band wait queue. */
			struct evl_wait_queue wait_queue;
			/*
			 * In-band side: Linux wait queues, allocated
			 * apart since the in-band kernel writes them.
			 */
			struct evl_monitor_ib *ib;
			/* Z -> z wake ups, record only (doc 24). */
			struct evl_inband_notify notify_r;
			struct evl_inband_notify notify_w;
			/* Gate (valid during active wait only). */
			struct evl_monitor *gate;
			/* Out-of-band poll head. */
			struct evl_poll_head poll_head;
			/* in ->events */
			struct list_head next;
		};
	};
};

long evl_functl_monitor(struct evl_monitor *mon,
		unsigned int cmd, unsigned long arg);

int evl_signal_monitor_targeted(struct evl_thread *target,
				__u32 eventfun);

void __evl_commit_monitor_ceiling(void);

static inline void evl_commit_monitor_ceiling(void)
{
	struct evl_thread *curr = evl_current();

	if (curr->sstate->pp_pending != EVL_NO_HANDLE)
		__evl_commit_monitor_ceiling();
}

#endif /* !_EVL_MONITOR_H */

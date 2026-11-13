/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_WORK_H
#define _EVL_WORK_H

#include <linux/workqueue.h>
#include <evl/flag.h>
#include <evl/inband_notify.h>

struct evl_element;

/*
 * Calling in-band from the oob stage: the trusted side only posts a
 * notification record (doc 24), the in-band handler of the relay
 * queues wq_work. wq_work is still a Linux object embedded here.
 */
struct evl_work {
	struct evl_inband_notify kick;
	struct work_struct wq_work;
	struct workqueue_struct *wq;
	union {
		int (*handler)(void *arg);
		void (*handler_noreturn)(void *arg);
	};
	struct evl_element *element;
};

#define EVL_DEFINE_WORK(__work, __handler)				\
	struct evl_work __work = {					\
		.kick = EVL_INBAND_NOTIFY_INIT(EVL_NOTIFY_CALL, 0,	\
					&(__work), __evl_do_inband_kick), \
		.wq_work = __WORK_INITIALIZER((__work).wq_work,		\
					__evl_do_work),			\
		.wq = NULL,						\
		.handler_noreturn = (void (*)(void *))__handler,	\
		.element = NULL,					\
	}

struct evl_sync_work {
	struct evl_work work;
	struct evl_flag done;
	int result;
};

#define EVL_DEFINE_SYNC_WORK(__work, __handler)				\
	struct evl_sync_work __work = {					\
		.work = {						\
			.kick = EVL_INBAND_NOTIFY_INIT(EVL_NOTIFY_CALL,	\
					0, &(__work).work,		\
					__evl_do_inband_kick),		\
			.wq_work = __WORK_INITIALIZER((__work).work.wq_work, \
						__evl_do_sync_work),	\
			.wq = NULL,					\
			.handler = (int (*)(void *))__handler,		\
			.element = NULL,				\
		},							\
		.done = EVL_FLAG_INITIALIZER((__work).done),		\
		.result = 0,						\
	}

void evl_init_work(struct evl_work *work,
		   void (*handler)(struct evl_work *work));

void evl_init_work_safe(struct evl_work *work,
			void (*handler)(struct evl_work *work),
			struct evl_element *element);

void evl_init_sync_work(struct evl_sync_work *sync_work,
			int (*handler)(struct evl_sync_work *sync_work));

bool evl_call_inband_from(struct evl_work *work,
			struct workqueue_struct *wq);

static inline bool evl_call_inband(struct evl_work *work)
{
	return evl_call_inband_from(work, system_wq);
}

static inline
void evl_flush_work(struct evl_work *work)
{
	evl_inband_notify_drain(&work->kick);
	flush_work(&work->wq_work);
}

static inline
void evl_cancel_work(struct evl_work *work)
{
	evl_inband_notify_drain(&work->kick);
	cancel_work_sync(&work->wq_work);
}

int evl_call_inband_sync_from(struct evl_sync_work *sync_work,
			struct workqueue_struct *wq);

static inline
int evl_call_inband_sync(struct evl_sync_work *sync_work)
{
	return evl_call_inband_sync_from(sync_work, system_wq);
}

static inline
void evl_flush_sync_work(struct evl_sync_work *sync_work)
{
	evl_flush_work(&sync_work->work);
}

void __evl_do_inband_kick(const struct evl_notify_event *ev);
void __evl_do_work(struct work_struct *wq_work);
void __evl_do_sync_work(struct work_struct *wq_work);

#endif /* !_EVL_WORK_H */

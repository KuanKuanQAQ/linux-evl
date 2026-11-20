// SPDX-License-Identifier: GPL-2.0
/* Linux in-band host for the separately linked, NOMMU LKL kernel. */
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/reboot.h>
#include <linux/sched/task_stack.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include <evl/control.h>
#include <evl/lkl.h>
#include "lkl_host_abi.h"

struct lkl_sem {
	struct semaphore sem;
};

struct lkl_mutex {
	struct mutex lock;
	struct task_struct *owner;
	unsigned int depth;
	bool recursive;
};

struct host_thread {
	struct list_head next;
	struct task_struct *task;
	struct completion exited;
	void (*fn)(void *);
	void *arg;
};

struct host_tls_value {
	struct list_head next;
	struct task_struct *task;
	void *data;
};

struct lkl_tls_key {
	struct list_head next;
	struct list_head values;
	void (*destructor)(void *);
};

struct host_timer {
	struct hrtimer timer;
	struct semaphore wake;
	struct mutex lock;
	void (*fn)(void);
	bool stopping;
};

static LIST_HEAD(host_threads);
static DEFINE_MUTEX(host_threads_lock);
static LIST_HEAD(host_tls_keys);
static DEFINE_MUTEX(host_tls_lock);
static DECLARE_COMPLETION(boot_done);
static DECLARE_COMPLETION(stop_request);
static struct task_struct *control_thread;
static int boot_result;
static bool runtime_started;
static bool lkl_boot = true;
static bool lkl_probe_passed;
static long lkl_probe_pid;
static unsigned long lkl_probe_sleep_ns;
static atomic_long_t timer_irqs = ATOMIC_LONG_INIT(0);

#ifdef CONFIG_EVL_LKL_CLOCK
static unsigned long lkl_clock_bootstrap_calls;
static unsigned long lkl_clock_host_time_calls;
static unsigned long lkl_clock_bootstrap_uncertainty_ns;
module_param_named(lkl_clock_bootstrap_calls, lkl_clock_bootstrap_calls, ulong, 0444);
module_param_named(lkl_clock_host_time_calls, lkl_clock_host_time_calls, ulong, 0444);
module_param_named(lkl_clock_bootstrap_uncertainty_ns,
		   lkl_clock_bootstrap_uncertainty_ns, ulong, 0444);

struct clock_cpu_setup {
	int (*setup)(void *);
	void *arg;
	int result;
};

static void host_clock_setup_cpu(void *arg)
{
	struct clock_cpu_setup *setup = arg;

	setup->result = setup->setup(setup->arg);
}

static int host_clock_for_each_cpu(int (*setup)(void *), void *arg)
{
	struct clock_cpu_setup call = { .setup = setup, .arg = arg };
	int cpu, ret = 0;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		ret = smp_call_function_single(cpu, host_clock_setup_cpu, &call, true);
		if (!ret)
			ret = call.result;
		if (ret)
			break;
	}
	cpus_read_unlock();
	return ret;
}

static void *host_clock_map_ram(unsigned long long physical, unsigned long size)
{
	if (!size || physical + size < physical ||
	    !pfn_valid(PHYS_PFN(physical)) ||
	    !pfn_valid(PHYS_PFN(physical + size - 1)))
		return NULL;
	return __va(physical);
}

/* The only host time sampling in the new clock path, called once at boot.
 * The bracket bounds the sampling error without exposing Linux internals.
 * The returned counter is in the device's own nanosecond domain.
 */
static int host_clock_bootstrap(unsigned long long (*read_counter)(void),
				struct lkl_clock_bootstrap *best)
{
	struct lkl_clock_bootstrap sample;
	struct ktime_timestamps times;
	unsigned long flags;
	u64 before, after;
	unsigned int n;

	if (lkl_clock_bootstrap_calls++)
		return -EALREADY;
	best->uncertainty_ns = U64_MAX;
	for (n = 0; n < 64; n++) {
		flags = hard_local_irq_save();
		before = read_counter();
		ktime_get_fast_timestamps(&times);
		sample.raw_ns = ktime_get_raw_fast_ns();
		after = read_counter();
		hard_local_irq_restore(flags);
		if (after < before)
			return -ERANGE;
		sample.counter_ns = before + (after - before) / 2;
		sample.mono_ns = times.mono;
		sample.real_ns = times.real;
		sample.uncertainty_ns = after - before;
		if (sample.uncertainty_ns < best->uncertainty_ns)
			*best = sample;
	}
	lkl_clock_bootstrap_uncertainty_ns = best->uncertainty_ns;
	return best->uncertainty_ns <= 100000 ? 0 : -ERANGE;
}
#endif

module_param_named(lkl_boot, lkl_boot, bool, 0444);
MODULE_PARM_DESC(lkl_boot, "Boot embedded LKL before initializing EVL");
module_param_named(lkl_probe_passed, lkl_probe_passed, bool, 0444);
module_param_named(lkl_probe_pid, lkl_probe_pid, long, 0444);
module_param_named(lkl_probe_sleep_ns, lkl_probe_sleep_ns, ulong, 0444);

static int running_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", lkl_is_running());
}

static int timer_irqs_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&timer_irqs));
}

static const struct kernel_param_ops running_ops = { .get = running_get };
static const struct kernel_param_ops timer_irqs_ops = { .get = timer_irqs_get };
module_param_cb(lkl_running, &running_ops, NULL, 0444);
module_param_cb(lkl_timer_irqs, &timer_irqs_ops, NULL, 0444);

static void host_print(const char *str, int len)
{
	printk(KERN_INFO "LKL: %.*s", len, str);
}

static void host_panic(void)
{
	panic("EVL: embedded LKL panic");
}

static struct lkl_sem *host_sem_alloc(int count)
{
	struct lkl_sem *sem = kmalloc(sizeof(*sem), GFP_KERNEL);

	if (sem)
		sema_init(&sem->sem, count);
	return sem;
}

static void host_sem_free(struct lkl_sem *sem)
{
	kfree(sem);
}

static void host_sem_up(struct lkl_sem *sem)
{
	up(&sem->sem);
}

static void host_sem_down(struct lkl_sem *sem)
{
	down(&sem->sem);
}

static struct lkl_mutex *host_mutex_alloc(int recursive)
{
	struct lkl_mutex *lock = kzalloc(sizeof(*lock), GFP_KERNEL);

	if (lock) {
		mutex_init(&lock->lock);
		lock->recursive = recursive;
	}
	return lock;
}

static void host_mutex_free(struct lkl_mutex *lock)
{
	kfree(lock);
}

static void host_mutex_lock(struct lkl_mutex *lock)
{
	if (lock->recursive && READ_ONCE(lock->owner) == current) {
		lock->depth++;
		return;
	}
	mutex_lock(&lock->lock);
	WRITE_ONCE(lock->owner, current);
	lock->depth = 1;
}

static void host_mutex_unlock(struct lkl_mutex *lock)
{
	if (WARN_ON(lock->owner != current || !lock->depth))
		return;
	if (--lock->depth)
		return;
	WRITE_ONCE(lock->owner, NULL);
	mutex_unlock(&lock->lock);
}

static struct lkl_tls_key *host_tls_alloc(void (*destructor)(void *))
{
	struct lkl_tls_key *key = kzalloc(sizeof(*key), GFP_KERNEL);

	if (!key)
		return NULL;
	INIT_LIST_HEAD(&key->values);
	key->destructor = destructor;
	mutex_lock(&host_tls_lock);
	list_add(&key->next, &host_tls_keys);
	mutex_unlock(&host_tls_lock);
	return key;
}

static void host_tls_free(struct lkl_tls_key *key)
{
	struct host_tls_value *value, *tmp;

	mutex_lock(&host_tls_lock);
	list_del(&key->next);
	list_for_each_entry_safe(value, tmp, &key->values, next) {
		list_del(&value->next);
		kfree(value);
	}
	mutex_unlock(&host_tls_lock);
	kfree(key);
}

static int host_tls_set(struct lkl_tls_key *key, void *data)
{
	struct host_tls_value *value;
	int ret = 0;

	mutex_lock(&host_tls_lock);
	list_for_each_entry(value, &key->values, next) {
		if (value->task == current)
			goto found;
	}
	value = kzalloc(sizeof(*value), GFP_KERNEL);
	if (!value) {
		ret = -ENOMEM;
		goto out;
	}
	value->task = current;
	list_add(&value->next, &key->values);
found:
	value->data = data;
out:
	mutex_unlock(&host_tls_lock);
	return ret;
}

static void *host_tls_get(struct lkl_tls_key *key)
{
	struct host_tls_value *value;
	void *data = NULL;

	mutex_lock(&host_tls_lock);
	list_for_each_entry(value, &key->values, next) {
		if (value->task == current) {
			data = value->data;
			break;
		}
	}
	mutex_unlock(&host_tls_lock);
	return data;
}

/* Call destructors without our lock: they may enter LKL and block. */
static void host_tls_exit(void)
{
	struct lkl_tls_key *key;
	struct host_tls_value *value;
	void (*destructor)(void *);
	void *data;

again:
	mutex_lock(&host_tls_lock);
	list_for_each_entry(key, &host_tls_keys, next) {
		list_for_each_entry(value, &key->values, next) {
			if (value->task != current)
				continue;
			data = value->data;
			destructor = key->destructor;
			list_del(&value->next);
			kfree(value);
			mutex_unlock(&host_tls_lock);
			if (data && destructor)
				destructor(data);
			goto again;
		}
	}
	mutex_unlock(&host_tls_lock);
}

static void __noreturn host_thread_exit(void)
{
	struct host_thread *thread;
	struct completion *exited = NULL;

	host_tls_exit();
	mutex_lock(&host_threads_lock);
	list_for_each_entry(thread, &host_threads, next) {
		if (thread->task == current) {
			exited = &thread->exited;
			break;
		}
	}
	mutex_unlock(&host_threads_lock);
	BUG_ON(!exited);
	kthread_complete_and_exit(exited, 0);
}

static int host_thread_main(void *arg)
{
	struct host_thread *thread = arg;

	thread->fn(thread->arg);
	host_thread_exit();
}

static lkl_thread_t host_thread_create(void (*fn)(void *), void *arg)
{
	struct host_thread *thread;
	struct task_struct *task;

	thread = kzalloc(sizeof(*thread), GFP_KERNEL);
	if (!thread)
		return 0;
	init_completion(&thread->exited);
	thread->fn = fn;
	thread->arg = arg;
	task = kthread_create(host_thread_main, thread, "evl-lkl");
	if (IS_ERR(task)) {
		kfree(thread);
		return 0;
	}
	get_task_struct(task);
	thread->task = task;
	mutex_lock(&host_threads_lock);
	list_add_tail(&thread->next, &host_threads);
	mutex_unlock(&host_threads_lock);
	wake_up_process(task);
	return (lkl_thread_t)task;
}

static int host_thread_join(lkl_thread_t tid)
{
	struct host_thread *thread, *found = NULL;

	if ((struct task_struct *)tid == current)
		return -EDEADLK;
	mutex_lock(&host_threads_lock);
	list_for_each_entry(thread, &host_threads, next) {
		if (thread->task == (struct task_struct *)tid) {
			found = thread;
			break;
		}
	}
	mutex_unlock(&host_threads_lock);
	if (!found)
		return -ESRCH;
	wait_for_completion(&found->exited);
	mutex_lock(&host_threads_lock);
	list_del(&found->next);
	mutex_unlock(&host_threads_lock);
	put_task_struct(found->task);
	kfree(found);
	return 0;
}

static void host_thread_detach(void)
{
	/* Retain the handle until host shutdown; never stop a live LKL thread. */
}

static lkl_thread_t host_thread_self(void)
{
	return (lkl_thread_t)current;
}

static int host_thread_equal(lkl_thread_t a, lkl_thread_t b)
{
	return a == b;
}

static void *host_thread_stack(unsigned long *size)
{
	if (size)
		*size = THREAD_SIZE;
	return task_stack_page(current);
}

static unsigned long long host_time(void)
{
#ifdef CONFIG_EVL_LKL_CLOCK
	lkl_clock_host_time_calls++;
	return lkl_clock_read_counter();
#else
	return ktime_get_ns();
#endif
}

static u64 probe_time(void)
{
#ifdef CONFIG_EVL_LKL_CLOCK
	return lkl_ktime_get_mono_fast_ns();
#else
	return host_time();
#endif
}

/* Hard IRQs only wake a host thread; LKL IRQ delivery may sleep. */
static enum hrtimer_restart host_timer_expire(struct hrtimer *hrtimer)
{
	struct host_timer *timer = container_of(hrtimer, struct host_timer, timer);

	up(&timer->wake);
	return HRTIMER_NORESTART;
}

static void host_timer_main(void *arg)
{
	struct host_timer *timer = arg;
	bool stopping;

	for (;;) {
		down(&timer->wake);
		mutex_lock(&timer->lock);
		stopping = timer->stopping;
		mutex_unlock(&timer->lock);
		if (stopping)
			break;
		atomic_long_inc(&timer_irqs);
		timer->fn();
	}
	kfree(timer);
}

static void *host_timer_alloc(void (*fn)(void))
{
	struct host_timer *timer = kzalloc(sizeof(*timer), GFP_KERNEL);

	if (!timer)
		return NULL;
	sema_init(&timer->wake, 0);
	mutex_init(&timer->lock);
	timer->fn = fn;
	hrtimer_init(&timer->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->timer.function = host_timer_expire;
	if (!host_thread_create(host_timer_main, timer)) {
		kfree(timer);
		return NULL;
	}
	return timer;
}

static int host_timer_set(void *arg, unsigned long delta)
{
	struct host_timer *timer = arg;

	hrtimer_start(&timer->timer, ns_to_ktime(delta), HRTIMER_MODE_REL);
	return 0;
}

static void host_timer_free(void *arg)
{
	struct host_timer *timer = arg;

	hrtimer_cancel(&timer->timer);
	mutex_lock(&timer->lock);
	timer->stopping = true;
	up(&timer->wake);
	mutex_unlock(&timer->lock);
	/* The worker frees its timer. Joining here could deadlock on LKL's CPU. */
}

static void *host_mem_alloc(unsigned long size)
{
	return kvmalloc(size, GFP_KERNEL);
}

static void host_mem_free(void *addr)
{
	kvfree(addr);
}

static void *host_page_alloc(unsigned long size)
{
	/* NOMMU LKL uses a contiguous virtual range as its own physical memory. */
	return vzalloc(size);
}

static void host_page_free(void *addr, unsigned long size)
{
	vfree(addr);
}

void evl_lkl_jmp_set(struct lkl_jmp_buf *buf, void (*fn)(void));
void evl_lkl_jmp_longjmp(struct lkl_jmp_buf *buf, int value);

static struct lkl_host_operations host_ops = {
	.print = host_print,
	.panic = host_panic,
	.sem_alloc = host_sem_alloc,
	.sem_free = host_sem_free,
	.sem_up = host_sem_up,
	.sem_down = host_sem_down,
	.mutex_alloc = host_mutex_alloc,
	.mutex_free = host_mutex_free,
	.mutex_lock = host_mutex_lock,
	.mutex_unlock = host_mutex_unlock,
	.thread_create = host_thread_create,
	.thread_detach = host_thread_detach,
	.thread_exit = host_thread_exit,
	.thread_join = host_thread_join,
	.thread_self = host_thread_self,
	.thread_equal = host_thread_equal,
	.thread_stack = host_thread_stack,
	.tls_alloc = host_tls_alloc,
	.tls_free = host_tls_free,
	.tls_set = host_tls_set,
	.tls_get = host_tls_get,
	.mem_alloc = host_mem_alloc,
	.mem_free = host_mem_free,
	.page_alloc = host_page_alloc,
	.page_free = host_page_free,
	.time = host_time,
	.timer_alloc = host_timer_alloc,
	.timer_set_oneshot = host_timer_set,
	.timer_free = host_timer_free,
	.jmp_buf_set = evl_lkl_jmp_set,
	.jmp_buf_longjmp = evl_lkl_jmp_longjmp,
	.memcpy = memcpy,
	.memset = memset,
	.memmove = memmove,
#ifdef CONFIG_EVL_LKL_CLOCK
	.clock_for_each_cpu = host_clock_for_each_cpu,
	.clock_map_ram = host_clock_map_ram,
	.clock_bootstrap = host_clock_bootstrap,
#endif
};

static long host_syscall(long nr, long a0, long a1, long a2, long a3)
{
	long args[6] = { a0, a1, a2, a3, 0, 0 };

	return lkl_syscall(nr, args);
}

static int runtime_probe(void)
{
	/* This is the 64-bit NOMMU LKL syscall ABI, not the outer x86 syscall ABI. */
	struct { s64 sec, nsec; } delay = { 0, 20000000 }, before, after;
	static const char path[] = "/evl-host-probe";
	static const char payload[] = "embedded LKL filesystem probe";
	char buffer[sizeof(payload)] = { 0 };
	long ret, fd;
	u64 start = probe_time();

	lkl_probe_pid = host_syscall(EVL_LKL_NR_getpid, 0, 0, 0, 0);
	if (lkl_probe_pid <= 0)
		return -EIO;
	ret = host_syscall(EVL_LKL_NR_clock_gettime, CLOCK_MONOTONIC,
			(long)&before, 0, 0);
	if (ret)
		return ret;
	ret = host_syscall(EVL_LKL_NR_nanosleep, (long)&delay, 0, 0, 0);
	if (ret)
		return ret;
	lkl_probe_sleep_ns = probe_time() - start;
	ret = host_syscall(EVL_LKL_NR_clock_gettime, CLOCK_MONOTONIC,
			(long)&after, 0, 0);
	if (ret)
		return ret;
	if ((after.sec - before.sec) * NSEC_PER_SEC + after.nsec - before.nsec <
			 delay.nsec || !atomic_long_read(&timer_irqs))
		return -EIO;

	fd = host_syscall(EVL_LKL_NR_openat, AT_FDCWD, (long)path,
			O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		return fd;
	ret = host_syscall(EVL_LKL_NR_write, fd, (long)payload, sizeof(payload), 0);
	host_syscall(EVL_LKL_NR_close, fd, 0, 0, 0);
	if (ret != sizeof(payload)) {
		if (ret >= 0)
			ret = -EIO;
		goto fail;
	}
	fd = host_syscall(EVL_LKL_NR_openat, AT_FDCWD, (long)path, O_RDONLY, 0);
	if (fd < 0) {
		ret = fd;
		goto fail;
	}
	ret = host_syscall(EVL_LKL_NR_read, fd, (long)buffer, sizeof(buffer), 0);
	host_syscall(EVL_LKL_NR_close, fd, 0, 0, 0);
	if (ret != sizeof(payload) || memcmp(buffer, payload, sizeof(payload))) {
		ret = -EIO;
		goto fail;
	}
	ret = 0;
fail:
	host_syscall(EVL_LKL_NR_unlinkat, AT_FDCWD, (long)path, 0, 0);
	return ret > 0 ? -EIO : ret;
}

static void runtime_control(void *unused)
{
	int ret;

	ret = lkl_init(&host_ops);
	if (ret)
		goto done;
	ret = lkl_start_kernel("mem=64M");
	if (ret)
		goto cleanup;
	if (!lkl_is_running()) {
		ret = -EIO;
		goto halt;
	}
#ifdef CONFIG_EVL_LKL_CLOCK
	if (!lkl_clock_is_ready()) {
		ret = -ENODEV;
		goto halt;
	}
#endif
	ret = runtime_probe();
	if (ret)
		goto halt;
	lkl_probe_passed = true;
	pr_info("EVL: embedded LKL started, runtime_running=%d pid=%ld sleep_ns=%lu timer_irqs=%ld probes=passed\n",
		lkl_is_running(), lkl_probe_pid, lkl_probe_sleep_ns,
		atomic_long_read(&timer_irqs));
	boot_result = 0;
	complete(&boot_done);
	wait_for_completion(&stop_request);
halt:
	if (lkl_is_running()) {
		long err = lkl_sys_halt();

		pr_info("EVL: embedded LKL halted, ret=%ld runtime_running=%d\n",
			err, lkl_is_running());
	}
cleanup:
	lkl_cleanup();
done:
	if (!lkl_probe_passed) {
		boot_result = ret;
		complete(&boot_done);
	}
}

static void reap_host_threads(void)
{
	struct host_thread *thread;
	lkl_thread_t tid;

	for (;;) {
		mutex_lock(&host_threads_lock);
		if (list_empty(&host_threads)) {
			mutex_unlock(&host_threads_lock);
			break;
		}
		thread = list_first_entry(&host_threads, struct host_thread, next);
		tid = (lkl_thread_t)thread->task;
		mutex_unlock(&host_threads_lock);
		host_thread_join(tid);
	}
}

static int runtime_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
#ifdef CONFIG_EVL_LKL_CLOCK
	/* Stop all EVL time consumers before shutting their timekeeper down. */
	if (evl_is_running() && evl_stop_services()) {
		pr_err("EVL: cannot stop services, retaining the LKL clock runtime\n");
		return NOTIFY_BAD;
	}
#endif
	evl_lkl_stop_runtime();
	return NOTIFY_DONE;
}

static struct notifier_block reboot_nb = { .notifier_call = runtime_reboot };

int evl_lkl_start_runtime(void)
{
	int ret;

	if (!lkl_boot) {
		pr_info("EVL: embedded LKL boot disabled by evl.lkl_boot=0\n");
		return IS_ENABLED(CONFIG_EVL_LKL_CLOCK) ? -ENODEV : 0;
	}
	control_thread = (struct task_struct *)host_thread_create(runtime_control, NULL);
	if (!control_thread)
		return -ENOMEM;
	wait_for_completion(&boot_done);
	if (boot_result) {
		pr_err("EVL: embedded LKL boot/probes failed: %d\n", boot_result);
		host_thread_join((lkl_thread_t)control_thread);
		if (!lkl_is_running())
			reap_host_threads();
		return boot_result;
	}
	runtime_started = true;
	ret = register_reboot_notifier(&reboot_nb);
	if (ret)
		evl_lkl_stop_runtime();
	return ret;
}

void evl_lkl_stop_runtime(void)
{
	if (!runtime_started)
		return;
	runtime_started = false;
	complete(&stop_request);
	host_thread_join((lkl_thread_t)control_thread);
	reap_host_threads();
}

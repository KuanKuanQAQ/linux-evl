// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/export.h>
#include <evl/lkl.h>

/* Definitions live in the separately compiled LKL, not in host wrappers. */
#ifdef CONFIG_EVL_LKL_CLOCK
EXPORT_SYMBOL_GPL(lkl_ktime_get_mono_fast_ns);
EXPORT_SYMBOL_GPL(lkl_ktime_get_raw_fast_ns);
EXPORT_SYMBOL_GPL(lkl_ktime_get_real_fast_ns);
#endif

/* The complete LKL core expects these diagnostics from its host library. */
int lkl_printf(const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vprintk(fmt, args);
	va_end(args);
	return ret;
}

void lkl_bug(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
	panic("embedded LKL fatal error");
}

#ifdef CONFIG_EVL_LKL
int lkl_is_running(void);

static int __init evl_lkl_linked(void)
{
	pr_info("EVL: complete LKL core linked, runtime_running=%d\n",
		lkl_is_running());
	return 0;
}
core_initcall(evl_lkl_linked);
#endif

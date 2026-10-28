// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <evl/lkl.h>

int lkl_printf(const char *fmt, ...)
{
        va_list ap;
        int ret;

        va_start(ap, fmt);
        ret = vprintk(fmt, ap);
        va_end(ap);

        return ret;
}

void lkl_bug(const char *fmt, ...)
{
        va_list ap;

        va_start(ap, fmt);
        vprintk(fmt, ap);
        va_end(ap);
        BUG();
}

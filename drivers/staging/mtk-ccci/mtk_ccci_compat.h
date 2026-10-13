/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux 7.1 stand-ins for the kernel interfaces the stock MT6765 CCCI
 * sources were written against. Force-included into every mtk-ccci object.
 */
#ifndef __MTK_CCCI_COMPAT_H__
#define __MTK_CCCI_COMPAT_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/sched/clock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

/*
 * The stock driver found its registers and interrupts by index in the
 * vendor DT layout; the mainline node names them.
 */
static inline void __iomem *ccci_of_iomap_byname(struct device_node *np,
						 const char *name)
{
	int idx = of_property_match_string(np, "reg-names", name);

	return idx < 0 ? NULL : of_iomap(np, idx);
}

static inline unsigned int ccci_of_irq_byname(struct device_node *np,
					      const char *name)
{
	int irq = of_irq_get_byname(np, name);

	return irq > 0 ? irq : 0;
}

/* wall clock in the struct timeval layout the userspace ABI carries */
static inline void ccci_gettimeofday(struct __kernel_old_timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}

#endif /* __MTK_CCCI_COMPAT_H__ */

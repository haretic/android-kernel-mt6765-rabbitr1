/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MT_SYNC_WRITE_H
#define MT_SYNC_WRITE_H

#include <linux/io.h>

/* register write that has reached the device before the next access */
#define mt_reg_sync_writel(v, a) \
	do { \
		__raw_writel((v), (void __force __iomem *)((a))); \
		dsb(sy); \
	} while (0)

#define mt_reg_sync_writew(v, a) \
	do { \
		__raw_writew((v), (void __force __iomem *)((a))); \
		dsb(sy); \
	} while (0)

#define mt_reg_sync_writeb(v, a) \
	do { \
		__raw_writeb((v), (void __force __iomem *)((a))); \
		dsb(sy); \
	} while (0)

#endif

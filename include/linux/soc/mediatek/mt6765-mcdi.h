/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_MEDIATEK_MT6765_MCDI_H
#define __SOC_MEDIATEK_MT6765_MCDI_H

#include <linux/errno.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_MTK_MT6765_MCDI)
int mt6765_mcdi_suspend_pause(bool pause);
#else
static inline int mt6765_mcdi_suspend_pause(bool pause)
{
	return -ENODEV;
}
#endif

#endif

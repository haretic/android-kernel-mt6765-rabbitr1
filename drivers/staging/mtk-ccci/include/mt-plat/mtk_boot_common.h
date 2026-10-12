/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Boot mode values LK reports in /chosen/atag,boot, which ccci_modem.c
 * reads itself. The stock CCCI build sets DISABLE_MTK_BOOT_MODE, so the
 * vendor boot driver's query functions are not needed.
 */
#ifndef __MTK_BOOT_COMMON_H__
#define __MTK_BOOT_COMMON_H__

#include <linux/types.h>

enum boot_mode_t {
	NORMAL_BOOT = 0,
	META_BOOT = 1,
	RECOVERY_BOOT = 2,
	SW_REBOOT = 3,
	FACTORY_BOOT = 4,
	ADVMETA_BOOT = 5,
	ATE_FACTORY_BOOT = 6,
	ALARM_BOOT = 7,
	KERNEL_POWER_OFF_CHARGING_BOOT = 8,
	LOW_POWER_OFF_CHARGING_BOOT = 9,
	DONGLE_BOOT = 10,
	UNKNOWN_BOOT
};

#endif

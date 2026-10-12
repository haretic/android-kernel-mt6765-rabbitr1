/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stand-in for the MediaTek vendor DRM's display blank notifier, which
 * does not exist upstream. wmt_dev.c uses it to keep CONSYS powered while
 * the screen is on (the "LPBK" function) and to tell the firmware about
 * blank state. Without it CONSYS is powered only on demand, when Wi-Fi
 * or Bluetooth is turned on, and the firmware is told the screen is off.
 */
#ifndef __MTK_DISP_NOTIFY_H__
#define __MTK_DISP_NOTIFY_H__

#include <linux/notifier.h>

#define MTK_DISP_EARLY_EVENT_BLANK	0x00
#define MTK_DISP_EVENT_BLANK		0x01

#define MTK_DISP_BLANK_UNBLANK		0x00
#define MTK_DISP_BLANK_POWERDOWN	0x01

static inline int mtk_disp_notifier_register(const char *source,
					     struct notifier_block *nb)
{
	return 0;
}

static inline int mtk_disp_notifier_unregister(struct notifier_block *nb)
{
	return 0;
}

#endif /* __MTK_DISP_NOTIFY_H__ */

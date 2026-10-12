/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2016 MediaTek Inc.
 */

#ifndef __CCCI_RTC_H__
#define __CCCI_RTC_H__


/*
 * The stock kernel read this from an "external-32k" RTC nvmem cell through
 * a mediatek,md_ccci_rtc node, which the Rabbit r1 DT never had, so it always
 * reported 0 (no 32 kHz crystal) to the modem. Keep that.
 */
static inline int mtk_crystal_exist_status(void)
{
	return 0;
}


#endif /* __CCCI_RTC_H__ */

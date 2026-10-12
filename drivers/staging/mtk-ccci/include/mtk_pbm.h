/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek power budget management is not available; the modem only
 * reported its power state to it.
 */
#ifndef __MTK_CCCI_PBM_H__
#define __MTK_CCCI_PBM_H__

#include <linux/types.h>

enum pbm_kicker {
	KR_DLPT,
	KR_MD1,
	KR_MD3,
	KR_CPU,
	KR_GPU,
	KR_FLASH,
};

static inline void kicker_pbm_by_md(enum pbm_kicker kicker, bool status)
{
}

static inline void init_md_section_level(enum pbm_kicker kicker)
{
}

#endif

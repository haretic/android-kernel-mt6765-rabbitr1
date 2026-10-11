/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6765 audio driver definitions
 *
 * The MT6765 AFE is a cut-down MT6771/MT8183 AFE: the register map, memory
 * interface fields, IRQ layout and ADDA (MTKAIF) block are the same, so this
 * driver reuses the MT8183 register and interconnection definitions.
 */

#ifndef _MT6765_AFE_COMMON_H_
#define _MT6765_AFE_COMMON_H_

#include <linux/list.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#include "../common/mtk-base-afe.h"

enum {
	MT6765_MEMIF_DL1,
	MT6765_MEMIF_DL2,
	MT6765_MEMIF_VUL,
	MT6765_MEMIF_AWB,
	MT6765_MEMIF_NUM,
	MT6765_DAI_ADDA = MT6765_MEMIF_NUM,
	MT6765_DAI_NUM,
};

enum {
	MT6765_IRQ_0,
	MT6765_IRQ_1,
	MT6765_IRQ_2,
	MT6765_IRQ_3,
	MT6765_IRQ_NUM,
};

struct clk;

struct mt6765_afe_private {
	struct clk **clk;
	int pm_runtime_bypass_reg_ctl;
};

unsigned int mt6765_general_rate_transform(struct device *dev,
					   unsigned int rate);

int mt6765_dai_adda_register(struct mtk_base_afe *afe);

int mt6765_init_clock(struct mtk_base_afe *afe);
int mt6765_afe_enable_clock(struct mtk_base_afe *afe);
void mt6765_afe_disable_clock(struct mtk_base_afe *afe);
int mt6765_afe_enable_audsys_clock(struct mtk_base_afe *afe);
void mt6765_afe_disable_audsys_clock(struct mtk_base_afe *afe);

#endif

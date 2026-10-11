// SPDX-License-Identifier: GPL-2.0
//
// MediaTek MT6765 AFE clock control
//
// Based on the MT8183 AFE clock control by KaiChieh Chuang.

#include <linux/clk.h>

#include "mt6765-afe-common.h"

enum {
	CLK_AFE = 0,
	CLK_DAC,
	CLK_DAC_PREDIS,
	CLK_ADC,
	CLK_INFRA_SYS_AUDIO,
	CLK_INFRA_AUDIO_26M,
	CLK_MUX_AUDIO,
	CLK_MUX_AUDIOINTBUS,
	CLK_TOP_SYSPLL1_D4,
	CLK_CLK26M,
	CLK_NUM
};

static const char *aud_clks[CLK_NUM] = {
	[CLK_AFE] = "aud_afe_clk",
	[CLK_DAC] = "aud_dac_clk",
	[CLK_DAC_PREDIS] = "aud_dac_predis_clk",
	[CLK_ADC] = "aud_adc_clk",
	[CLK_INFRA_SYS_AUDIO] = "aud_infra_clk",
	[CLK_INFRA_AUDIO_26M] = "mtkaif_26m_clk",
	[CLK_MUX_AUDIO] = "top_mux_audio",
	[CLK_MUX_AUDIOINTBUS] = "top_mux_aud_intbus",
	[CLK_TOP_SYSPLL1_D4] = "top_syspll1_d4",
	[CLK_CLK26M] = "top_clk26m_clk",
};

/* AUDIO_TOP_CON0 power-down gates, owned by the audsys clock driver */
static const int audsys_clks[] = {
	CLK_AFE, CLK_DAC, CLK_DAC_PREDIS, CLK_ADC,
};

int mt6765_init_clock(struct mtk_base_afe *afe)
{
	struct mt6765_afe_private *afe_priv = afe->platform_priv;
	int i;

	afe_priv->clk = devm_kcalloc(afe->dev, CLK_NUM, sizeof(*afe_priv->clk),
				     GFP_KERNEL);
	if (!afe_priv->clk)
		return -ENOMEM;

	for (i = 0; i < CLK_NUM; i++) {
		afe_priv->clk[i] = devm_clk_get(afe->dev, aud_clks[i]);
		if (IS_ERR(afe_priv->clk[i]))
			return dev_err_probe(afe->dev, PTR_ERR(afe_priv->clk[i]),
					     "failed to get clock %s\n",
					     aud_clks[i]);
	}

	return 0;
}

/* Bus and interface clocks; the AFE registers are reachable once these run. */
int mt6765_afe_enable_clock(struct mtk_base_afe *afe)
{
	struct mt6765_afe_private *afe_priv = afe->platform_priv;
	struct clk **clk = afe_priv->clk;
	int ret;

	ret = clk_prepare_enable(clk[CLK_INFRA_SYS_AUDIO]);
	if (ret)
		goto err_infra;

	ret = clk_prepare_enable(clk[CLK_MUX_AUDIO]);
	if (ret)
		goto err_mux_audio;

	ret = clk_set_parent(clk[CLK_MUX_AUDIO], clk[CLK_CLK26M]);
	if (ret)
		goto err_mux_intbus;

	ret = clk_prepare_enable(clk[CLK_MUX_AUDIOINTBUS]);
	if (ret)
		goto err_mux_intbus;

	ret = clk_set_parent(clk[CLK_MUX_AUDIOINTBUS], clk[CLK_TOP_SYSPLL1_D4]);
	if (ret)
		goto err_infra_26m;

	ret = clk_prepare_enable(clk[CLK_INFRA_AUDIO_26M]);
	if (ret)
		goto err_infra_26m;

	return 0;

err_infra_26m:
	clk_disable_unprepare(clk[CLK_MUX_AUDIOINTBUS]);
err_mux_intbus:
	clk_disable_unprepare(clk[CLK_MUX_AUDIO]);
err_mux_audio:
	clk_disable_unprepare(clk[CLK_INFRA_SYS_AUDIO]);
err_infra:
	dev_err(afe->dev, "%s(), failed to enable clocks: %d\n", __func__, ret);
	return ret;
}

void mt6765_afe_disable_clock(struct mtk_base_afe *afe)
{
	struct mt6765_afe_private *afe_priv = afe->platform_priv;
	struct clk **clk = afe_priv->clk;

	clk_disable_unprepare(clk[CLK_INFRA_AUDIO_26M]);
	clk_disable_unprepare(clk[CLK_MUX_AUDIOINTBUS]);
	clk_disable_unprepare(clk[CLK_MUX_AUDIO]);
	clk_disable_unprepare(clk[CLK_INFRA_SYS_AUDIO]);
}

/*
 * The audsys gates live in AUDIO_TOP_CON0, a volatile register of the shared
 * regmap. Toggle them only while the regmap is out of cache-only mode, or the
 * gate update is dropped.
 */
int mt6765_afe_enable_audsys_clock(struct mtk_base_afe *afe)
{
	struct mt6765_afe_private *afe_priv = afe->platform_priv;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(audsys_clks); i++) {
		ret = clk_prepare_enable(afe_priv->clk[audsys_clks[i]]);
		if (ret) {
			dev_err(afe->dev, "%s(), failed to enable %s: %d\n",
				__func__, aud_clks[audsys_clks[i]], ret);
			while (--i >= 0)
				clk_disable_unprepare(afe_priv->clk[audsys_clks[i]]);
			return ret;
		}
	}

	return 0;
}

void mt6765_afe_disable_audsys_clock(struct mtk_base_afe *afe)
{
	struct mt6765_afe_private *afe_priv = afe->platform_priv;
	int i;

	for (i = ARRAY_SIZE(audsys_clks) - 1; i >= 0; i--)
		clk_disable_unprepare(afe_priv->clk[audsys_clks[i]]);
}

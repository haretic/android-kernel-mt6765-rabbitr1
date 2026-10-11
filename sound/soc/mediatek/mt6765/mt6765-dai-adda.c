// SPDX-License-Identifier: GPL-2.0
//
// MediaTek MT6765 ADDA DAI: the AFE side of the MTKAIF link to the MT6357
//
// Based on the MT8183 ADDA DAI by KaiChieh Chuang. The MT6765 talks MTKAIF
// protocol 1 to the MT6357, so no phase calibration is needed.

#include <linux/delay.h>
#include <linux/regmap.h>

#include "mt6765-afe-common.h"
#include "../mt8183/mt8183-interconnection.h"
#include "../mt8183/mt8183-reg.h"
#include "../common/mtk-dai-adda-common.h"

#define AUDIO_SDM_LEVEL_NORMAL	0x1d

static const struct snd_kcontrol_new mtk_adda_dl_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN3, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN3, I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN3,
				    I_ADDA_UL_CH1, 1, 0),
};

static const struct snd_kcontrol_new mtk_adda_dl_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN4, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN4, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN4, I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN4, I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN4,
				    I_ADDA_UL_CH2, 1, 0),
};

static int mtk_adda_ul_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	/* should delay 1/fs (smallest is 8k) = 125us before AFE off */
	if (event == SND_SOC_DAPM_POST_PMD)
		usleep_range(125, 135);

	return 0;
}

enum {
	SUPPLY_SEQ_ADDA_AFE_ON,
	SUPPLY_SEQ_ADDA_DL_ON,
	SUPPLY_SEQ_ADDA_UL_ON,
};

static const struct snd_soc_dapm_widget mtk_dai_adda_widgets[] = {
	SND_SOC_DAPM_MIXER("ADDA_DL_CH1", SND_SOC_NOPM, 0, 0,
			   mtk_adda_dl_ch1_mix,
			   ARRAY_SIZE(mtk_adda_dl_ch1_mix)),
	SND_SOC_DAPM_MIXER("ADDA_DL_CH2", SND_SOC_NOPM, 0, 0,
			   mtk_adda_dl_ch2_mix,
			   ARRAY_SIZE(mtk_adda_dl_ch2_mix)),

	SND_SOC_DAPM_SUPPLY_S("ADDA Enable", SUPPLY_SEQ_ADDA_AFE_ON,
			      AFE_ADDA_UL_DL_CON0, ADDA_AFE_ON_SFT, 0,
			      NULL, 0),

	SND_SOC_DAPM_SUPPLY_S("ADDA Playback Enable", SUPPLY_SEQ_ADDA_DL_ON,
			      AFE_ADDA_DL_SRC2_CON0,
			      DL_2_SRC_ON_TMP_CTL_PRE_SFT, 0,
			      NULL, 0),

	SND_SOC_DAPM_SUPPLY_S("ADDA Capture Enable", SUPPLY_SEQ_ADDA_UL_ON,
			      AFE_ADDA_UL_SRC_CON0,
			      UL_SRC_ON_TMP_CTL_SFT, 0,
			      mtk_adda_ul_event,
			      SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route mtk_dai_adda_routes[] = {
	/* playback */
	{"ADDA_DL_CH1", "DL1_CH1", "DL1"},
	{"ADDA_DL_CH2", "DL1_CH1", "DL1"},
	{"ADDA_DL_CH2", "DL1_CH2", "DL1"},

	{"ADDA_DL_CH1", "DL2_CH1", "DL2"},
	{"ADDA_DL_CH2", "DL2_CH1", "DL2"},
	{"ADDA_DL_CH2", "DL2_CH2", "DL2"},

	{"ADDA Playback", NULL, "ADDA_DL_CH1"},
	{"ADDA Playback", NULL, "ADDA_DL_CH2"},

	/* adda enable */
	{"ADDA Playback", NULL, "ADDA Enable"},
	{"ADDA Playback", NULL, "ADDA Playback Enable"},
	{"ADDA Capture", NULL, "ADDA Enable"},
	{"ADDA Capture", NULL, "ADDA Capture Enable"},
};

static int mtk_dai_adda_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int rate = params_rate(params);

	dev_dbg(afe->dev, "%s(), id %d, stream %d, rate %d\n",
		__func__, dai->id, substream->stream, rate);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		unsigned int dl_src2_con0;

		/* clean predistortion */
		regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON0, 0);
		regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON1, 0);

		/* input rate, 8x up-sampling, mute off */
		dl_src2_con0 = mtk_adda_dl_rate_transform(afe, rate) << 28;
		dl_src2_con0 |= 0x3 << 24;
		dl_src2_con0 |= 0x3 << 11;

		/* voice mode input for 8k / 16k */
		if (rate == 8000 || rate == 16000)
			dl_src2_con0 |= 0x1 << 5;

		/* down-link gain on */
		dl_src2_con0 |= 0x1 << 1;

		regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0, dl_src2_con0);
		/* -0.3 dB, as the vendor driver applies to the audio path */
		regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON1, 0xf74f0000);

		regmap_update_bits(afe->regmap, AFE_ADDA_DL_SDM_DCCOMP_CON,
				   ATTGAIN_CTL_MASK_SFT,
				   AUDIO_SDM_LEVEL_NORMAL << ATTGAIN_CTL_SFT);
	} else {
		unsigned int ul_src_con0;

		/* MTKAIF protocol 1, receive FIFO on */
		regmap_write(afe->regmap, AFE_AUD_PAD_TOP, 0x31);
		regmap_write(afe->regmap, AFE_ADDA_MTKAIF_CFG0, 0x0);

		/* internal ADC */
		regmap_update_bits(afe->regmap, AFE_ADDA_TOP_CON0, 0x1, 0x0);

		ul_src_con0 = (mtk_adda_ul_rate_transform(afe, rate) << 17) &
			      (0x7 << 17);
		ul_src_con0 |= (1 << UL_IIR_ON_TMP_CTL_SFT) &
			       UL_IIR_ON_TMP_CTL_MASK_SFT;

		/* 35Hz @ 48k high-pass */
		regmap_write(afe->regmap, AFE_ADDA_IIR_COEF_02_01, 0x00000000);
		regmap_write(afe->regmap, AFE_ADDA_IIR_COEF_04_03, 0x00003FB8);
		regmap_write(afe->regmap, AFE_ADDA_IIR_COEF_06_05, 0x3FB80000);
		regmap_write(afe->regmap, AFE_ADDA_IIR_COEF_08_07, 0x3FB80000);
		regmap_write(afe->regmap, AFE_ADDA_IIR_COEF_10_09, 0x0000C048);

		regmap_write(afe->regmap, AFE_ADDA_UL_SRC_CON0, ul_src_con0);

		/* mtkaif_rxif_data_mode = 0, amic */
		regmap_update_bits(afe->regmap, AFE_ADDA_MTKAIF_RX_CFG0, 0x1, 0x0);
	}

	return 0;
}

static const struct snd_soc_dai_ops mtk_dai_adda_ops = {
	.hw_params = mtk_dai_adda_hw_params,
};

#define MTK_ADDA_PLAYBACK_RATES SNDRV_PCM_RATE_8000_48000

#define MTK_ADDA_CAPTURE_RATES (SNDRV_PCM_RATE_8000 |\
				SNDRV_PCM_RATE_16000 |\
				SNDRV_PCM_RATE_32000 |\
				SNDRV_PCM_RATE_48000)

#define MTK_ADDA_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			  SNDRV_PCM_FMTBIT_S24_LE |\
			  SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mtk_dai_adda_driver[] = {
	{
		.name = "ADDA",
		.id = MT6765_DAI_ADDA,
		.playback = {
			.stream_name = "ADDA Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_PLAYBACK_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.capture = {
			.stream_name = "ADDA Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_CAPTURE_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.ops = &mtk_dai_adda_ops,
	},
};

int mt6765_dai_adda_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mtk_dai_adda_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_adda_driver);
	dai->dapm_widgets = mtk_dai_adda_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mtk_dai_adda_widgets);
	dai->dapm_routes = mtk_dai_adda_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_adda_routes);

	return 0;
}

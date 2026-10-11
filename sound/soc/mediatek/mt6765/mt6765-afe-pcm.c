// SPDX-License-Identifier: GPL-2.0
//
// MediaTek ALSA SoC AFE platform driver for MT6765
//
// Based on the MT8183 AFE platform driver by KaiChieh Chuang. The MT6765 AFE
// shares the MT8183 register layout; this driver exposes the subset that the
// MT6765 has and that phones built on it use: DL1/DL2 playback and VUL/AWB
// capture memory interfaces in front of the ADDA link to the MT6357.

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "mt6765-afe-common.h"
#include "../mt8183/mt8183-interconnection.h"
#include "../mt8183/mt8183-reg.h"
#include "../common/mtk-afe-fe-dai.h"
#include "../common/mtk-afe-platform-driver.h"

enum {
	MTK_AFE_RATE_8K = 0,
	MTK_AFE_RATE_11K = 1,
	MTK_AFE_RATE_12K = 2,
	MTK_AFE_RATE_384K = 3,
	MTK_AFE_RATE_16K = 4,
	MTK_AFE_RATE_22K = 5,
	MTK_AFE_RATE_24K = 6,
	MTK_AFE_RATE_130K = 7,
	MTK_AFE_RATE_32K = 8,
	MTK_AFE_RATE_44K = 9,
	MTK_AFE_RATE_48K = 10,
	MTK_AFE_RATE_88K = 11,
	MTK_AFE_RATE_96K = 12,
	MTK_AFE_RATE_176K = 13,
	MTK_AFE_RATE_192K = 14,
	MTK_AFE_RATE_260K = 15,
};

unsigned int mt6765_general_rate_transform(struct device *dev,
					   unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_RATE_8K;
	case 11025:
		return MTK_AFE_RATE_11K;
	case 12000:
		return MTK_AFE_RATE_12K;
	case 16000:
		return MTK_AFE_RATE_16K;
	case 22050:
		return MTK_AFE_RATE_22K;
	case 24000:
		return MTK_AFE_RATE_24K;
	case 32000:
		return MTK_AFE_RATE_32K;
	case 44100:
		return MTK_AFE_RATE_44K;
	case 48000:
		return MTK_AFE_RATE_48K;
	case 88200:
		return MTK_AFE_RATE_88K;
	case 96000:
		return MTK_AFE_RATE_96K;
	case 176400:
		return MTK_AFE_RATE_176K;
	case 192000:
		return MTK_AFE_RATE_192K;
	default:
		dev_warn(dev, "%s(), rate %u invalid, use 48k\n", __func__, rate);
		return MTK_AFE_RATE_48K;
	}
}

static const struct snd_pcm_hardware mt6765_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		   SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min = 256,
	.period_bytes_max = 4 * 48 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.buffer_bytes_max = 8 * 48 * 1024,
	.fifo_size = 0,
};

static int mt6765_memif_fs(struct snd_pcm_substream *substream,
			   unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	return mt6765_general_rate_transform(afe->dev, rate);
}

#define MTK_PCM_RATES (SNDRV_PCM_RATE_8000_48000 |\
		       SNDRV_PCM_RATE_88200 |\
		       SNDRV_PCM_RATE_96000 |\
		       SNDRV_PCM_RATE_176400 |\
		       SNDRV_PCM_RATE_192000)

#define MTK_PCM_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mt6765_memif_dai_driver[] = {
	/* FE DAIs: memory interfaces to CPU */
	{
		.name = "DL1",
		.id = MT6765_MEMIF_DL1,
		.playback = {
			.stream_name = "DL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "DL2",
		.id = MT6765_MEMIF_DL2,
		.playback = {
			.stream_name = "DL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL1",
		.id = MT6765_MEMIF_VUL,
		.capture = {
			.stream_name = "UL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL2",
		.id = MT6765_MEMIF_AWB,
		.capture = {
			.stream_name = "UL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
};

/* dma widget & routes */
static const struct snd_kcontrol_new memif_ul1_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN9,
				    I_ADDA_UL_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul1_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN10,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN5,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN5,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN5,
				    I_DL2_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN6,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN6,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN6,
				    I_DL2_CH2, 1, 0),
};

static const struct snd_soc_dapm_widget mt6765_memif_widgets[] = {
	SND_SOC_DAPM_MIXER("UL1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch1_mix, ARRAY_SIZE(memif_ul1_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL1_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch2_mix, ARRAY_SIZE(memif_ul1_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch1_mix, ARRAY_SIZE(memif_ul2_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL2_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch2_mix, ARRAY_SIZE(memif_ul2_ch2_mix)),
};

static const struct snd_soc_dapm_route mt6765_memif_routes[] = {
	/* capture */
	{"UL1", NULL, "UL1_CH1"},
	{"UL1", NULL, "UL1_CH2"},
	{"UL1_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL1_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL2", NULL, "UL2_CH1"},
	{"UL2", NULL, "UL2_CH2"},
	{"UL2_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL2_CH2", "ADDA_UL_CH2", "ADDA Capture"},
	{"UL2_CH1", "DL1_CH1", "DL1"},
	{"UL2_CH2", "DL1_CH2", "DL1"},
	{"UL2_CH1", "DL2_CH1", "DL2"},
	{"UL2_CH2", "DL2_CH2", "DL2"},
};

static const struct snd_soc_component_driver mt6765_afe_pcm_dai_component = {
	.name = "mt6765-afe-pcm-dai",
};

#define MT6765_MEMIF(_id) \
	[MT6765_MEMIF_##_id] = { \
		.name = #_id,				\
		.id = MT6765_MEMIF_##_id,		\
		.reg_ofs_base = AFE_##_id##_BASE,	\
		.reg_ofs_cur = AFE_##_id##_CUR,		\
		.reg_ofs_end = AFE_##_id##_END,		\
		.reg_ofs_base_msb = AFE_##_id##_BASE_MSB,	\
		.reg_ofs_cur_msb = AFE_##_id##_CUR_MSB,		\
		.reg_ofs_end_msb = AFE_##_id##_END_MSB,		\
		.fs_reg = AFE_DAC_CON1,			\
		.fs_shift = _id##_MODE_SFT,		\
		.fs_maskbit = _id##_MODE_MASK,		\
		.mono_reg = AFE_DAC_CON1,		\
		.mono_shift = _id##_DATA_SFT,		\
		.enable_reg = AFE_DAC_CON0,		\
		.enable_shift = _id##_ON_SFT,		\
		.hd_reg = AFE_MEMIF_HD_MODE,		\
		.hd_align_reg = AFE_MEMIF_HDALIGN,	\
		.hd_shift = _id##_HD_SFT,		\
		.hd_align_mshift = _id##_HD_ALIGN_SFT,	\
		.agent_disable_reg = -1,		\
		.agent_disable_shift = -1,		\
		.msb_reg = -1,				\
		.msb_shift = -1,			\
	}

/* Same fields as the MT6765 vendor driver's memif tables */
static const struct mtk_base_memif_data memif_data[MT6765_MEMIF_NUM] = {
	MT6765_MEMIF(DL1),
	MT6765_MEMIF(DL2),
	MT6765_MEMIF(VUL),
	MT6765_MEMIF(AWB),
};

#define MT6765_AFE_IRQ(_id)	\
	[MT6765_IRQ_##_id] = {	\
		.id = MT6765_IRQ_##_id,			\
		.irq_cnt_reg = AFE_IRQ_MCU_CNT##_id,	\
		.irq_cnt_shift = 0,			\
		.irq_cnt_maskbit = 0x3ffff,		\
		.irq_fs_reg = AFE_IRQ_MCU_CON1,		\
		.irq_fs_shift = IRQ##_id##_MCU_MODE_SFT,	\
		.irq_fs_maskbit = IRQ##_id##_MCU_MODE_MASK,	\
		.irq_en_reg = AFE_IRQ_MCU_CON0,		\
		.irq_en_shift = IRQ##_id##_MCU_ON_SFT,	\
		.irq_clr_reg = AFE_IRQ_MCU_CLR,		\
		.irq_clr_shift = IRQ##_id##_MCU_CLR_SFT,	\
	}

static const struct mtk_base_irq_data irq_data[MT6765_IRQ_NUM] = {
	MT6765_AFE_IRQ(0),
	MT6765_AFE_IRQ(1),
	MT6765_AFE_IRQ(2),
	MT6765_AFE_IRQ(3),
};

static bool mt6765_is_volatile_reg(struct device *dev, unsigned int reg)
{
	/* these auto-gen reg has read-only bit, so put it as volatile */
	/* volatile reg cannot be cached, so cannot be set when power off */
	switch (reg) {
	case AUDIO_TOP_CON0 ... AUDIO_TOP_CON1:	/* reg bit controlled by CCF */
	case AUDIO_TOP_CON3:
	case AFE_DL1_CUR ... AFE_DL1_END:
	case AFE_DL2_CUR ... AFE_DL2_END:
	case AFE_AWB_END ... AFE_AWB_CUR:
	case AFE_VUL_END ... AFE_VUL_CUR:
	case AFE_MEMIF_MON0 ... AFE_MEMIF_MON9:
	case AFE_ADDA_SRC_DEBUG_MON0 ... AFE_ADDA_SRC_DEBUG_MON1:
	case AFE_ADDA_UL_SRC_MON0 ... AFE_ADDA_UL_SRC_MON1:
	case AFE_SIDETONE_MON:
	case AFE_SIDETONE_CON0 ... AFE_SIDETONE_COEFF:
	case AFE_BUS_MON0:
	case AFE_MRGIF_MON0 ... AFE_I2S_MON:
	case AFE_DAC_MON:
	case AFE_VUL2_END ... AFE_VUL2_CUR:
	case AFE_IRQ0_MCU_CNT_MON ... AFE_IRQ6_MCU_CNT_MON:
	case AFE_MOD_DAI_END ... AFE_MOD_DAI_CUR:
	case AFE_IRQ3_MCU_CNT_MON ... AFE_IRQ4_MCU_CNT_MON:
	case AFE_IRQ_MCU_STATUS ... AFE_IRQ_MCU_CLR:
	case AFE_IRQ_MCU_MON2:
	case AFE_IRQ1_MCU_CNT_MON ... AFE_IRQ5_MCU_CNT_MON:
	case AFE_IRQ7_MCU_CNT_MON:
	case AFE_GAIN1_CUR:
	case AFE_GAIN2_CUR:
	case AFE_SRAM_DELSEL_CON0:
	case AFE_SRAM_DELSEL_CON2 ... AFE_SRAM_DELSEL_CON3:
	case AFE_ASRC_2CH_CON12 ... AFE_ASRC_2CH_CON13:
	case PCM_INTF_CON2:
	case FPGA_CFG0 ... FPGA_CFG1:
	case FPGA_CFG2 ... FPGA_CFG3:
	case AUDIO_TOP_DBG_MON0 ... AUDIO_TOP_DBG_MON1:
	case AFE_IRQ8_MCU_CNT_MON ... AFE_IRQ12_MCU_CNT_MON:
	case AFE_CBIP_MON0:
	case AFE_CBIP_SLV_MUX_MON0 ... AFE_CBIP_SLV_DECODER_MON0:
	case AFE_DL1_CUR_MSB:
	case AFE_DL2_CUR_MSB:
	case AFE_AWB_CUR_MSB:
	case AFE_VUL_CUR_MSB:
	case AFE_VUL2_CUR_MSB:
	case AFE_MOD_DAI_CUR_MSB:
	case AFE_AWB2_END ... AFE_AWB2_CUR:
	case AFE_AWB2_CUR_MSB:
	case AFE_ADDA_DL_SDM_FIFO_MON ... AFE_ADDA_DL_SDM_OUT_MON:
	case AFE_CONNSYS_I2S_MON ... AFE_ASRC_2CH_CON0:
	case AFE_ASRC_2CH_CON2 ... AFE_ASRC_2CH_CON5:
	case AFE_ASRC_2CH_CON7 ... AFE_ASRC_2CH_CON8:
	case AFE_MEMIF_MON12 ... AFE_MEMIF_MON24:
	case AFE_ADDA_MTKAIF_MON0 ... AFE_ADDA_MTKAIF_MON1:
	case AFE_AUD_PAD_TOP:
	case AFE_GENERAL1_ASRC_2CH_CON0:
	case AFE_GENERAL1_ASRC_2CH_CON2 ... AFE_GENERAL1_ASRC_2CH_CON5:
	case AFE_GENERAL1_ASRC_2CH_CON7 ... AFE_GENERAL1_ASRC_2CH_CON8:
	case AFE_GENERAL1_ASRC_2CH_CON12 ... AFE_GENERAL1_ASRC_2CH_CON13:
	case AFE_GENERAL2_ASRC_2CH_CON0:
	case AFE_GENERAL2_ASRC_2CH_CON2 ... AFE_GENERAL2_ASRC_2CH_CON5:
	case AFE_GENERAL2_ASRC_2CH_CON7 ... AFE_GENERAL2_ASRC_2CH_CON8:
	case AFE_GENERAL2_ASRC_2CH_CON12 ... AFE_GENERAL2_ASRC_2CH_CON13:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config mt6765_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,

	.volatile_reg = mt6765_is_volatile_reg,

	.max_register = AFE_MAX_REGISTER,
	.num_reg_defaults_raw = AFE_MAX_REGISTER,

	.cache_type = REGCACHE_FLAT,
};

static irqreturn_t mt6765_afe_irq_handler(int irq_id, void *dev)
{
	struct mtk_base_afe *afe = dev;
	struct mtk_base_afe_irq *irq;
	unsigned int status;
	unsigned int status_mcu;
	unsigned int mcu_en;
	int ret;
	int i;
	irqreturn_t irq_ret = IRQ_HANDLED;

	/* get irq that is sent to MCU */
	regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &mcu_en);

	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	/* only care IRQ which is sent to MCU */
	status_mcu = status & mcu_en & AFE_IRQ_STATUS_BITS;

	if (ret || status_mcu == 0) {
		dev_err(afe->dev, "%s(), irq status err, ret %d, status 0x%x, mcu_en 0x%x\n",
			__func__, ret, status, mcu_en);

		irq_ret = IRQ_NONE;
		goto err_irq;
	}

	for (i = 0; i < MT6765_MEMIF_NUM; i++) {
		struct mtk_base_afe_memif *memif = &afe->memif[i];

		if (!memif->substream)
			continue;

		if (memif->irq_usage < 0)
			continue;

		irq = &afe->irqs[memif->irq_usage];

		if (status_mcu & (1 << irq->irq_data->irq_en_shift))
			snd_pcm_period_elapsed(memif->substream);
	}

err_irq:
	/* clear irq */
	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, status_mcu);

	return irq_ret;
}

static int mt6765_afe_runtime_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	struct mt6765_afe_private *afe_priv = afe->platform_priv;
	unsigned int value;
	int ret;

	if (!afe->regmap || afe_priv->pm_runtime_bypass_reg_ctl)
		goto skip_regmap;

	/* disable AFE */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_ON_MASK_SFT, 0x0);

	ret = regmap_read_poll_timeout(afe->regmap, AFE_DAC_MON, value,
				       (value & AFE_ON_RETM_MASK_SFT) == 0,
				       20, 1 * 1000 * 1000);
	if (ret)
		dev_warn(afe->dev, "%s(), ret %d\n", __func__, ret);

	/* make sure all irq status are cleared, twice intended */
	regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CLR, 0xffff, 0xffff);
	regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CLR, 0xffff, 0xffff);

	mt6765_afe_disable_audsys_clock(afe);

	/* cache only */
	regcache_cache_only(afe->regmap, true);
	regcache_mark_dirty(afe->regmap);

	mt6765_afe_disable_clock(afe);
	return 0;

skip_regmap:
	mt6765_afe_disable_audsys_clock(afe);
	mt6765_afe_disable_clock(afe);
	return 0;
}

static int mt6765_afe_runtime_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	struct mt6765_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = mt6765_afe_enable_clock(afe);
	if (ret)
		return ret;

	if (!afe->regmap || afe_priv->pm_runtime_bypass_reg_ctl) {
		ret = mt6765_afe_enable_audsys_clock(afe);
		if (ret)
			mt6765_afe_disable_clock(afe);
		return ret;
	}

	regcache_cache_only(afe->regmap, false);

	ret = mt6765_afe_enable_audsys_clock(afe);
	if (ret) {
		regcache_cache_only(afe->regmap, true);
		mt6765_afe_disable_clock(afe);
		return ret;
	}

	regcache_sync(afe->regmap);

	/* enable audio sys DCM for power saving */
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON0, 0x1 << 29, 0x1 << 29);

	/* force cpu use 8_24 format when writing 32bit data */
	regmap_update_bits(afe->regmap, AFE_MEMIF_MSB,
			   CPU_HD_ALIGN_MASK_SFT, 0 << CPU_HD_ALIGN_SFT);

	/* set all output port to 24bit */
	regmap_write(afe->regmap, AFE_CONN_24BIT, 0xffffffff);
	regmap_write(afe->regmap, AFE_CONN_24BIT_1, 0xffffffff);

	/* enable AFE */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0, 0x1, 0x1);

	return 0;
}

static int mt6765_dai_memif_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mt6765_memif_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mt6765_memif_dai_driver);
	dai->dapm_widgets = mt6765_memif_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mt6765_memif_widgets);
	dai->dapm_routes = mt6765_memif_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt6765_memif_routes);

	return 0;
}

typedef int (*dai_register_cb)(struct mtk_base_afe *);
static const dai_register_cb dai_register_cbs[] = {
	mt6765_dai_adda_register,
	mt6765_dai_memif_register,
};

static int mt6765_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct mtk_base_afe *afe;
	struct mt6765_afe_private *afe_priv;
	struct device *dev = &pdev->dev;
	struct reset_control *rstc;
	int i, irq_id, ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	if (ret)
		return ret;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	platform_set_drvdata(pdev, afe);

	afe->platform_priv = devm_kzalloc(dev, sizeof(*afe_priv), GFP_KERNEL);
	if (!afe->platform_priv)
		return -ENOMEM;

	afe_priv = afe->platform_priv;
	afe->dev = dev;

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_info(dev, "no reserved memory found, pre-allocating buffers instead\n");
		afe->preallocate_buffers = true;
	}

	ret = mt6765_init_clock(afe);
	if (ret)
		return ret;

	pm_runtime_enable(dev);

	/* regmap init */
	afe->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(afe->regmap)) {
		dev_err(dev, "could not get regmap from parent\n");
		ret = PTR_ERR(afe->regmap);
		goto err_pm_disable;
	}
	ret = regmap_attach_dev(dev, afe->regmap, &mt6765_afe_regmap_config);
	if (ret) {
		dev_warn(dev, "regmap_attach_dev fail, ret %d\n", ret);
		goto err_pm_disable;
	}

	rstc = devm_reset_control_get_optional_exclusive(dev, "audiosys");
	if (IS_ERR(rstc)) {
		ret = dev_err_probe(dev, PTR_ERR(rstc),
				    "could not get audiosys reset\n");
		goto err_pm_disable;
	}

	ret = reset_control_reset(rstc);
	if (ret) {
		dev_err(dev, "failed to trigger audio reset:%d\n", ret);
		goto err_pm_disable;
	}

	/* enable clock for regcache get default value from hw */
	afe_priv->pm_runtime_bypass_reg_ctl = true;
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto err_pm_disable;

	ret = regmap_reinit_cache(afe->regmap, &mt6765_afe_regmap_config);
	pm_runtime_put_sync(dev);
	afe_priv->pm_runtime_bypass_reg_ctl = false;
	if (ret) {
		dev_err(dev, "regmap_reinit_cache fail, ret %d\n", ret);
		goto err_pm_disable;
	}

	regcache_cache_only(afe->regmap, true);
	regcache_mark_dirty(afe->regmap);

	/* init memif */
	afe->memif_size = MT6765_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif) {
		ret = -ENOMEM;
		goto err_pm_disable;
	}

	for (i = 0; i < afe->memif_size; i++) {
		afe->memif[i].data = &memif_data[i];
		afe->memif[i].irq_usage = -1;
	}

	mutex_init(&afe->irq_alloc_lock);

	/* irq initialize */
	afe->irqs_size = MT6765_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);
	if (!afe->irqs) {
		ret = -ENOMEM;
		goto err_pm_disable;
	}

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &irq_data[i];

	irq_id = platform_get_irq(pdev, 0);
	if (irq_id < 0) {
		ret = irq_id;
		goto err_pm_disable;
	}

	ret = devm_request_irq(dev, irq_id, mt6765_afe_irq_handler,
			       IRQF_TRIGGER_NONE, "asys-isr", (void *)afe);
	if (ret) {
		dev_err(dev, "could not request_irq for asys-isr\n");
		goto err_pm_disable;
	}

	/* init sub_dais */
	INIT_LIST_HEAD(&afe->sub_dais);

	for (i = 0; i < ARRAY_SIZE(dai_register_cbs); i++) {
		ret = dai_register_cbs[i](afe);
		if (ret) {
			dev_warn(dev, "dai register i %d fail, ret %d\n",
				 i, ret);
			goto err_pm_disable;
		}
	}

	/* init dai_driver and component_driver */
	ret = mtk_afe_combine_sub_dai(afe);
	if (ret) {
		dev_warn(dev, "mtk_afe_combine_sub_dai fail, ret %d\n", ret);
		goto err_pm_disable;
	}

	afe->mtk_afe_hardware = &mt6765_afe_hardware;
	afe->memif_fs = mt6765_memif_fs;
	afe->irq_fs = mt6765_memif_fs;

	afe->runtime_resume = mt6765_afe_runtime_resume;
	afe->runtime_suspend = mt6765_afe_runtime_suspend;

	ret = devm_snd_soc_register_component(dev, &mtk_afe_pcm_platform,
					      NULL, 0);
	if (ret) {
		dev_warn(dev, "err_platform\n");
		goto err_pm_disable;
	}

	ret = devm_snd_soc_register_component(dev, &mt6765_afe_pcm_dai_component,
					      afe->dai_drivers,
					      afe->num_dai_drivers);
	if (ret) {
		dev_warn(dev, "err_dai_component\n");
		goto err_pm_disable;
	}

	return 0;

err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void mt6765_afe_pcm_dev_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		mt6765_afe_runtime_suspend(dev);
}

static const struct of_device_id mt6765_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt6765-audio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6765_afe_pcm_dt_match);

static const struct dev_pm_ops mt6765_afe_pm_ops = {
	RUNTIME_PM_OPS(mt6765_afe_runtime_suspend,
		       mt6765_afe_runtime_resume, NULL)
};

static struct platform_driver mt6765_afe_pcm_driver = {
	.driver = {
		   .name = "mt6765-audio",
		   .of_match_table = mt6765_afe_pcm_dt_match,
		   .pm = pm_ptr(&mt6765_afe_pm_ops),
	},
	.probe = mt6765_afe_pcm_dev_probe,
	.remove = mt6765_afe_pcm_dev_remove,
};

module_platform_driver(mt6765_afe_pcm_driver);

MODULE_DESCRIPTION("MediaTek ALSA SoC AFE platform driver for MT6765");
MODULE_LICENSE("GPL");

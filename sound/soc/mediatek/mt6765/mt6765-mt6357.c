// SPDX-License-Identifier: GPL-2.0
//
// MediaTek MT6765 sound card: AFE ADDA <-> MT6357 codec, with an optional
// analog speaker amplifier (e.g. AW87390) fed from the MT6357 line output.
//
// Based on the MT8365 EVK machine driver by Nicolas Belin.

#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>

#include <sound/soc.h>

#include "mt6765-afe-common.h"

enum {
	PIN_STATE_MOSI_OFF,
	PIN_STATE_MOSI_ON,
	PIN_STATE_MISO_OFF,
	PIN_STATE_MISO_ON,
	PIN_STATE_MAX
};

static const char * const mt6765_mt6357_pin_str[PIN_STATE_MAX] = {
	"aud_mosi_off",
	"aud_mosi_on",
	"aud_miso_off",
	"aud_miso_on",
};

struct mt6765_mt6357_priv {
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_states[PIN_STATE_MAX];
};

static int mt6765_mt6357_select_pins(struct snd_soc_pcm_runtime *rtd,
				     int stream, bool on)
{
	struct mt6765_mt6357_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	int state;
	int ret;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		state = on ? PIN_STATE_MOSI_ON : PIN_STATE_MOSI_OFF;
	else
		state = on ? PIN_STATE_MISO_ON : PIN_STATE_MISO_OFF;

	if (IS_ERR_OR_NULL(priv->pin_states[state]))
		return 0;

	ret = pinctrl_select_state(priv->pinctrl, priv->pin_states[state]);
	if (ret)
		dev_err(rtd->card->dev, "failed to select %s: %d\n",
			mt6765_mt6357_pin_str[state], ret);

	return ret;
}

static int mt6765_mt6357_adda_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	return mt6765_mt6357_select_pins(rtd, substream->stream, true);
}

static void mt6765_mt6357_adda_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	mt6765_mt6357_select_pins(rtd, substream->stream, false);
}

static const struct snd_soc_ops mt6765_mt6357_adda_ops = {
	.startup = mt6765_mt6357_adda_startup,
	.shutdown = mt6765_mt6357_adda_shutdown,
};

SND_SOC_DAILINK_DEFS(playback1,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(playback2,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(capture1,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(capture2,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(primary_codec,
		     DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
		     DAILINK_COMP_ARRAY(COMP_CODEC("mt6357-sound",
						   "mt6357-snd-codec-aif1")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

#define MT6765_FE_LINK(_name, _stream_name, _dir, _defs)	\
	{							\
		.name = _name,					\
		.stream_name = _stream_name,			\
		.trigger = {					\
			SND_SOC_DPCM_TRIGGER_PRE,		\
			SND_SOC_DPCM_TRIGGER_PRE		\
		},						\
		.dynamic = 1,					\
		._dir##_only = 1,				\
		.dpcm_merged_rate = 1,				\
		SND_SOC_DAILINK_REG(_defs),			\
	}

/* PCM device numbers follow the array order */
static struct snd_soc_dai_link mt6765_mt6357_dai_links[] = {
	/* Front End DAI links */
	MT6765_FE_LINK("Playback_1", "Playback_1", playback, playback1),
	MT6765_FE_LINK("Playback_2", "Playback_2", playback, playback2),
	MT6765_FE_LINK("Capture_1", "Capture_1", capture, capture1),
	MT6765_FE_LINK("Capture_2", "Capture_2", capture, capture2),
	/* Back End DAI links */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.ignore_suspend = 1,
		.ops = &mt6765_mt6357_adda_ops,
		SND_SOC_DAILINK_REG(primary_codec),
	},
};

static const struct snd_soc_dapm_widget mt6765_mt6357_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
};

static const struct snd_kcontrol_new mt6765_mt6357_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Main Mic"),
};

static const struct snd_soc_dapm_route mt6765_mt6357_routes[] = {
	{"Headphone", NULL, "Headphones"},
	{"AIN0", NULL, "Main Mic"},
};

/* The speaker amplifier sits between the codec line out and the speaker. */
static const struct snd_soc_dapm_route mt6765_mt6357_amp_routes[] = {
	{"Speaker Amp IN", NULL, "Line out"},
	{"Speaker", NULL, "Speaker Amp OUT"},
};

static const struct snd_soc_dapm_route mt6765_mt6357_lineout_routes[] = {
	{"Speaker", NULL, "Line out"},
};

static int mt6765_mt6357_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);

	if (card->num_aux_devs)
		return snd_soc_dapm_add_routes(dapm, mt6765_mt6357_amp_routes,
					       ARRAY_SIZE(mt6765_mt6357_amp_routes));

	return snd_soc_dapm_add_routes(dapm, mt6765_mt6357_lineout_routes,
				       ARRAY_SIZE(mt6765_mt6357_lineout_routes));
}

static struct snd_soc_card mt6765_mt6357_card = {
	.name = "mt6765-mt6357",
	/* Keeps the ALSA driver name (UCM conf.d key) when "model" renames */
	.driver_name = "mt6765-mt6357",
	.owner = THIS_MODULE,
	.dai_link = mt6765_mt6357_dai_links,
	.num_links = ARRAY_SIZE(mt6765_mt6357_dai_links),
	.controls = mt6765_mt6357_controls,
	.num_controls = ARRAY_SIZE(mt6765_mt6357_controls),
	.dapm_widgets = mt6765_mt6357_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6765_mt6357_widgets),
	.dapm_routes = mt6765_mt6357_routes,
	.num_dapm_routes = ARRAY_SIZE(mt6765_mt6357_routes),
	.late_probe = mt6765_mt6357_late_probe,
};

static void mt6765_mt6357_get_pins(struct device *dev,
				   struct mt6765_mt6357_priv *priv)
{
	int i;

	priv->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(priv->pinctrl)) {
		dev_info(dev, "no pinctrl, leaving the AUD pins as they are\n");
		return;
	}

	for (i = 0; i < PIN_STATE_MAX; i++) {
		priv->pin_states[i] = pinctrl_lookup_state(priv->pinctrl,
							   mt6765_mt6357_pin_str[i]);
		if (IS_ERR(priv->pin_states[i]))
			dev_info(dev, "no pin state %s\n",
				 mt6765_mt6357_pin_str[i]);
	}

	/* Park both directions until a stream needs them. */
	for (i = PIN_STATE_MOSI_OFF; i < PIN_STATE_MAX; i += 2)
		if (!IS_ERR(priv->pin_states[i]))
			pinctrl_select_state(priv->pinctrl, priv->pin_states[i]);
}

static int mt6765_mt6357_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt6765_mt6357_card;
	struct device *dev = &pdev->dev;
	struct device_node *platform_node, *amp_node;
	struct snd_soc_dai_link *dai_link;
	struct mt6765_mt6357_priv *priv;
	int i, ret;

	card->dev = dev;

	ret = snd_soc_of_parse_card_name(card, "model");
	if (ret)
		return ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_node = of_parse_phandle(dev->of_node, "mediatek,platform", 0);
	if (!platform_node)
		return dev_err_probe(dev, -EINVAL,
				     "Property 'mediatek,platform' missing or invalid\n");

	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->platforms->name)
			continue;
		dai_link->platforms->of_node = platform_node;
	}

	amp_node = of_parse_phandle(dev->of_node, "mediatek,speaker-amplifier", 0);
	if (amp_node) {
		struct snd_soc_aux_dev *aux;
		struct snd_soc_codec_conf *conf;

		aux = devm_kzalloc(dev, sizeof(*aux), GFP_KERNEL);
		conf = devm_kzalloc(dev, sizeof(*conf), GFP_KERNEL);
		if (!aux || !conf) {
			ret = -ENOMEM;
			goto err_put_nodes;
		}

		aux->dlc.of_node = amp_node;
		card->aux_dev = aux;
		card->num_aux_devs = 1;

		conf->dlc.of_node = amp_node;
		conf->name_prefix = "Speaker Amp";
		card->codec_conf = conf;
		card->num_configs = 1;
	}

	mt6765_mt6357_get_pins(dev, priv);
	snd_soc_card_set_drvdata(card, priv);

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		dev_err_probe(dev, ret, "failed to register card\n");

err_put_nodes:
	of_node_put(amp_node);
	of_node_put(platform_node);
	return ret;
}

static const struct of_device_id mt6765_mt6357_dt_match[] = {
	{ .compatible = "mediatek,mt6765-mt6357-sound", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6765_mt6357_dt_match);

static struct platform_driver mt6765_mt6357_driver = {
	.driver = {
		.name = "mt6765-mt6357",
		.of_match_table = mt6765_mt6357_dt_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = mt6765_mt6357_dev_probe,
};

module_platform_driver(mt6765_mt6357_driver);

MODULE_DESCRIPTION("MediaTek MT6765 with MT6357 codec ALSA SoC machine driver");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRAM frequency through the MediaTek DVFSRC.
 *
 * Once the DVFSRC is started its firmware runs the DRAM at the lowest OPP
 * nothing votes against, which on MT6765 halves the LPDDR4X data rate. The
 * vendor kernel raises it with CPU load (cm_mgr). This driver does the same
 * with the passive governor following cpufreq: each OPP is a DVFSRC level,
 * requested with MTK_DVFSRC_CMD_OPP.
 */

#include <linux/devfreq.h>
#include <linux/err.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/soc/mediatek/dvfsrc.h>

struct mtk_dvfsrc_devfreq {
	struct device *dvfsrc;
	struct devfreq_dev_profile profile;
	unsigned long cur_freq;
};

static int mtk_dvfsrc_devfreq_target(struct device *dev, unsigned long *freq,
				     u32 flags)
{
	struct mtk_dvfsrc_devfreq *drv = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned int level;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	level = dev_pm_opp_get_level(opp);
	dev_pm_opp_put(opp);

	if (*freq == drv->cur_freq)
		return 0;

	ret = mtk_dvfsrc_send_request(drv->dvfsrc, MTK_DVFSRC_CMD_OPP, level);
	if (ret)
		return ret;

	drv->cur_freq = *freq;
	return 0;
}

static int mtk_dvfsrc_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct mtk_dvfsrc_devfreq *drv = dev_get_drvdata(dev);

	*freq = drv->cur_freq;
	return 0;
}

static int mtk_dvfsrc_devfreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct devfreq_passive_data *passive;
	struct mtk_dvfsrc_devfreq *drv;
	struct devfreq *devfreq;
	struct dev_pm_opp *opp;
	unsigned long freq = 0;
	int ret;

	if (!dev->parent)
		return dev_err_probe(dev, -ENODEV, "no DVFSRC parent\n");

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	passive = devm_kzalloc(dev, sizeof(*passive), GFP_KERNEL);
	if (!drv || !passive)
		return -ENOMEM;
	drv->dvfsrc = dev->parent;
	platform_set_drvdata(pdev, drv);

	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return dev_err_probe(dev, ret, "no DRAM OPP table\n");

	/* Start from the lowest OPP, the level the DVFSRC runs without requests. */
	opp = dev_pm_opp_find_freq_ceil(dev, &freq);
	if (IS_ERR(opp))
		return dev_err_probe(dev, PTR_ERR(opp), "empty DRAM OPP table\n");
	ret = mtk_dvfsrc_send_request(drv->dvfsrc, MTK_DVFSRC_CMD_OPP,
				      dev_pm_opp_get_level(opp));
	dev_pm_opp_put(opp);
	if (ret)
		return dev_err_probe(dev, ret, "DVFSRC rejected the lowest level\n");
	drv->cur_freq = freq;

	drv->profile.initial_freq = freq;
	drv->profile.target = mtk_dvfsrc_devfreq_target;
	drv->profile.get_cur_freq = mtk_dvfsrc_devfreq_get_cur_freq;

	passive->parent_type = CPUFREQ_PARENT_DEV;
	devfreq = devm_devfreq_add_device(dev, &drv->profile, DEVFREQ_GOV_PASSIVE,
					  passive);
	if (IS_ERR(devfreq))
		return dev_err_probe(dev, PTR_ERR(devfreq), "cannot add devfreq\n");

	return 0;
}

static const struct of_device_id mtk_dvfsrc_devfreq_of_match[] = {
	{ .compatible = "mediatek,mt6765-dvfsrc-dram" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_dvfsrc_devfreq_of_match);

static struct platform_driver mtk_dvfsrc_devfreq_driver = {
	.probe = mtk_dvfsrc_devfreq_probe,
	.driver = {
		.name = "mtk-dvfsrc-devfreq",
		.of_match_table = mtk_dvfsrc_devfreq_of_match,
	},
};
module_platform_driver(mtk_dvfsrc_devfreq_driver);

MODULE_DESCRIPTION("MediaTek DVFSRC DRAM devfreq driver");
MODULE_LICENSE("GPL");

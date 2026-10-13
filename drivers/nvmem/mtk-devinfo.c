// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek eFuse words handed over by the bootloader.
 *
 * On SoCs such as the MT6765 the eFuse controller reads as zero from the
 * normal world once the secure boot chain has run. MediaTek's bootloader
 * (LK) copies the fuse words into the "atag,devinfo" property of /chosen
 * instead: a u32 word count followed by the words, in CPU byte order, where
 * word N is the fuse word at offset 4 * N of the eFuse block. This driver
 * serves the eFuse node's nvmem cells from that copy.
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

struct mtk_devinfo {
	const u32 *words;
	size_t size;
};

static int mtk_devinfo_read(void *context, unsigned int offset, void *val,
			    size_t bytes)
{
	struct mtk_devinfo *priv = context;

	if (offset >= priv->size || bytes > priv->size - offset)
		return -EINVAL;

	memcpy(val, (const u8 *)priv->words + offset, bytes);

	return 0;
}

static int mtk_devinfo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_config config = {};
	struct mtk_devinfo *priv;
	struct device_node *chosen;
	struct nvmem_device *nvmem;
	const u32 *prop;
	int len;
	u32 count;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return dev_err_probe(dev, -ENODEV, "no /chosen node\n");

	prop = of_get_property(chosen, "atag,devinfo", &len);
	of_node_put(chosen);
	if (!prop || len < sizeof(u32))
		return dev_err_probe(dev, -ENODEV,
				     "no atag,devinfo from the bootloader\n");

	/* The bootloader writes the property in CPU byte order. */
	count = prop[0];
	if (count > (len - sizeof(u32)) / sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "atag,devinfo claims %u words in %d bytes\n",
				     count, len);

	priv->words = prop + 1;
	priv->size = count * sizeof(u32);

	config.dev = dev;
	config.name = "mtk-devinfo";
	config.id = NVMEM_DEVID_AUTO;
	config.owner = THIS_MODULE;
	config.read_only = true;
	config.word_size = 1;
	config.stride = 1;
	config.size = priv->size;
	config.reg_read = mtk_devinfo_read;
	config.priv = priv;
	config.add_legacy_fixed_of_cells = true;

	nvmem = devm_nvmem_register(dev, &config);

	return PTR_ERR_OR_ZERO(nvmem);
}

static const struct of_device_id mtk_devinfo_of_match[] = {
	{ .compatible = "mediatek,mt6765-devinfo" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_devinfo_of_match);

static struct platform_driver mtk_devinfo_driver = {
	.probe = mtk_devinfo_probe,
	.driver = {
		.name = "mtk-devinfo",
		.of_match_table = mtk_devinfo_of_match,
	},
};
module_platform_driver(mtk_devinfo_driver);

MODULE_DESCRIPTION("MediaTek eFuse words from the bootloader's devinfo");
MODULE_LICENSE("GPL");

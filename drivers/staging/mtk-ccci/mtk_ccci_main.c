// SPDX-License-Identifier: GPL-2.0
/*
 * Module entry for the MT6765 CCCI port. The stock kernel built these
 * pieces in and ordered them with initcall levels; run them in that order.
 */

#include <linux/bits.h>
#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "mtk_ccci_main.h"

/* TOPCKGEN CLK_MODE: MD clock gating, cleared before MD power on */
#define TOPCKGEN_CLK_MODE		0x0
#define TOPCKGEN_MD_CLK_GATE		(BIT(8) | BIT(9))

static int __init mtk_ccci_init(void)
{
	struct regmap *topckgen;
	int ret;

	/* subsys_initcall level in the stock kernel */
	ret = ccci_util_init();
	if (ret)
		return ret;
	ret = ccci_init();
	if (ret)
		return ret;

	/* device_initcall level, in stock link order */
	ret = ccci_auxadc_init();
	if (ret)
		pr_warn("ccci: md auxadc unavailable (%d)\n", ret);

	/*
	 * Step 1 of md_cd_power_on(): MD clock mode. Done before the modem
	 * driver binds, because binding attaches and powers the MD1 domain.
	 * md_cd_power_off() sets the bits again.
	 */
	topckgen = syscon_regmap_lookup_by_compatible("mediatek,mt6765-topckgen");
	if (IS_ERR(topckgen))
		return PTR_ERR(topckgen);
	regmap_clear_bits(topckgen, TOPCKGEN_CLK_MODE, TOPCKGEN_MD_CLK_GATE);

	return modem_cd_init();
}
module_init(mtk_ccci_init);

/* The vendor stack has no teardown path; the module cannot be unloaded. */

MODULE_DESCRIPTION("MediaTek MT6765 CCCI modem interface (stock vendor port)");
MODULE_LICENSE("GPL");

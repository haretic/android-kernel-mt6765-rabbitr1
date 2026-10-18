// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6765 system suspend through the SPM ("deep" mem sleep).
 *
 * The stock ATF does not implement PSCI SYSTEM_SUSPEND usefully. The stock
 * kernel (slp_suspend_ops_enter / spm_go_to_sleep) instead hands the SPM PCM
 * firmware its suspend arguments and enters the "suspend" CPU_SUSPEND state
 * on the boot CPU with the other CPUs offline. The SPM firmware then puts
 * the SoC down (DRAM self-refresh, 26 MHz off, vcore low) and wakes it on one
 * of its wake sources; ATF owns that mask.
 *
 * LK loads the firmware image (the spmfw partition, one entry per DRAM type)
 * and hands it to ATF; the kernel only says which entry to run, from the
 * board's DRAM type and rate (mediatek,spmfw-idx). The DVFSRC driver's start
 * call then kicks it, and the suspend runs on it: without it running the SPM
 * never answers the suspend and its watchdog resets the board.
 *
 * The sequence follows the stock one:
 *   MCDI task pause, PCM watchdog on, SSPM SPM_SUSPEND notification,
 *   SPM_SUSPEND_ARGS, CPU_SUSPEND(suspend state), SSPM SPM_RESUME, PCM
 *   watchdog off, MCDI task resume.
 *
 * Around the sleep only the SPM interrupt stays enabled at the GIC, as stock
 * mtk_spm_irq_backup does: ATF's power-down path is "wfi; b .", so any other
 * interrupt that wakes the core from WFI parks it in that loop, the SPM never
 * sees the core idle and its watchdog resets the board.
 */

#include <linux/arm-smccc.h>
#include <linux/cpu_pm.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/sched/clock.h>
#include <linux/soc/mediatek/mt6765-mcdi.h>
#include <linux/suspend.h>
#include <asm/arch_timer.h>

#define MTK_SIP_SPM_SUSPEND_ARGS	0xc2000220
#define MTK_SIP_SPM_FIRMWARE_STATUS	0xc2000221
#define MTK_SIP_SPM_IRQ0_HANDLER	0xc2000222
#define MTK_SIP_SPM_ARGS		0xc200022a
#define MTK_SIP_SPM_GET_PWR_CTRL_ARGS	0xc200022b

/* MTK_SIP_SPM_FIRMWARE_STATUS: -1 not set up, 0 none, 1 loaded, 2 kicked */
#define SPM_FIRMWARE_LOADED		1
#define SPM_FIRMWARE_RUNNING		2

/* MTK_SIP_SPM_ARGS sub-commands */
#define SPM_ARGS_SPMFW_IDX		0
#define SPM_ARGS_PCM_WDT		8

/* MTK_SIP_SPM_GET_PWR_CTRL_ARGS */
#define SPM_PWR_CTRL_SUSPEND		17
#define PW_WAKE_SRC			12

/* pcm_flags: no vcore DVS/DFS, no ATF abort, suspend option (stock 0x80098) */
#define SPM_FLAG_SUSPEND		0x00080098
/* The stock wake period: the SPM timer wakes the SoC every 600 s at most. */
#define SPM_WAKE_PERIOD_S		600
#define SPM_PCM_WDT_S			30

/* The "suspend" state of the stock idle-states: cluster power-down, id 5. */
#define MT6765_PSCI_SUSPEND_STATE	0x01010005

/*
 * SSPM mailbox 1 carries IPI pins 5 to 10; SPM_SUSPEND is pin 6, its slots
 * start after the two of pin 5, and it raises IRQ bit 6 - 5.
 */
#define SSPM_SPM_SLOT			2
#define SSPM_SPM_WORDS			8
#define SSPM_SPM_IRQ			BIT(1)
#define SSPM_MBOX_IN_IRQ		0x0
#define SSPM_MBOX_OUT_IRQ		0x4
#define SSPM_SPM_SUSPEND		0
#define SSPM_SPM_RESUME			1

struct mt6765_spm_sleep {
	struct device *dev;
	void __iomem *mbox;
	void __iomem *mbox_ctrl;
	void __iomem *irq_sta;
	void __iomem *gicd;
	unsigned int irq;
	unsigned int gicd_regs;
	u32 *gicd_enable;
	u32 wake_src;
};

static struct mt6765_spm_sleep *spm_sleep;

static unsigned long spm_smc(unsigned long fid, unsigned long a1,
			     unsigned long a2, unsigned long a3,
			     unsigned long a4)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fid, a1, a2, a3, a4, 0, 0, 0, &res);
	return res.a0;
}

/* Stock spm_to_sspm_command: a polled IPI with the AP and SSPM timestamps. */
static int spm_sspm_command(struct mt6765_spm_sleep *spm, u32 cmd)
{
	u64 ns = sched_clock(), cnt = __arch_counter_get_cntvct();
	u32 data[SSPM_SPM_WORDS] = {
		cmd, lower_32_bits(ns), upper_32_bits(ns),
		lower_32_bits(cnt), upper_32_bits(cnt),
	};
	u32 irq;
	int i, ret;

	if (readl(spm->mbox_ctrl + SSPM_MBOX_OUT_IRQ) & SSPM_SPM_IRQ)
		return -EBUSY;

	for (i = 0; i < SSPM_SPM_WORDS; i++)
		writel(data[i], spm->mbox + (SSPM_SPM_SLOT + i) * 4);
	writel(SSPM_SPM_IRQ, spm->mbox_ctrl + SSPM_MBOX_IN_IRQ);

	ret = readl_poll_timeout_atomic(spm->mbox_ctrl + SSPM_MBOX_OUT_IRQ, irq,
					irq & SSPM_SPM_IRQ, 1, 2000000);
	if (ret)
		return ret;
	writel(SSPM_SPM_IRQ, spm->mbox_ctrl + SSPM_MBOX_OUT_IRQ);

	ret = readl(spm->mbox + SSPM_SPM_SLOT * 4);
	return ret < 0 ? -EIO : 0;
}

/* Leave only the SPM interrupt enabled; ISENABLER0 (SGIs, PPIs) is banked. */
static void spm_gic_mask(struct mt6765_spm_sleep *spm)
{
	unsigned int i;

	for (i = 1; i < spm->gicd_regs; i++) {
		spm->gicd_enable[i] = readl(spm->gicd + GICD_ISENABLER + i * 4);
		writel(spm->gicd_enable[i], spm->gicd + GICD_ICENABLER + i * 4);
	}
	writel(BIT(spm->irq % 32), spm->gicd + GICD_ISENABLER + spm->irq / 32 * 4);
}

static void spm_gic_restore(struct mt6765_spm_sleep *spm)
{
	unsigned int i;

	for (i = 1; i < spm->gicd_regs; i++) {
		writel(~spm->gicd_enable[i], spm->gicd + GICD_ICENABLER + i * 4);
		writel(spm->gicd_enable[i], spm->gicd + GICD_ISENABLER + i * 4);
	}
}

/* Stock spm_irq0_handler: ATF clears the SPM's IRQ status bits. */
static irqreturn_t mt6765_spm_irq0(int irq, void *data)
{
	struct mt6765_spm_sleep *spm = data;

	spm_smc(MTK_SIP_SPM_IRQ0_HANDLER, readl(spm->irq_sta), 0, 0, 0);

	return IRQ_HANDLED;
}

static int mt6765_spm_sleep_enter(suspend_state_t state)
{
	struct mt6765_spm_sleep *spm = spm_sleep;
	long status;
	int ret;

	status = spm_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0, 0);
	if (status != SPM_FIRMWARE_RUNNING) {
		dev_err(spm->dev, "SPM firmware not running (%ld)\n", status);
		return -ENODEV;
	}

	ret = mt6765_mcdi_suspend_pause(true);
	if (ret) {
		dev_err(spm->dev, "MCDI task did not pause: %d\n", ret);
		return ret;
	}

	spm_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_PCM_WDT, 1, SPM_PCM_WDT_S, 0);
	spm_gic_mask(spm);

	ret = spm_sspm_command(spm, SSPM_SPM_SUSPEND);
	if (ret) {
		dev_err(spm->dev, "SSPM did not take SPM_SUSPEND: %d\n", ret);
		goto out_wdt;
	}

	spm_smc(MTK_SIP_SPM_SUSPEND_ARGS, SPM_FLAG_SUSPEND, 0,
		SPM_WAKE_PERIOD_S << 15, 0);

	ret = cpu_pm_enter();
	if (!ret) {
		ret = psci_cpu_suspend_enter(MT6765_PSCI_SUSPEND_STATE);
		cpu_pm_exit();
	}
	if (ret)
		dev_err(spm->dev, "suspend state not entered: %d\n", ret);

	if (spm_sspm_command(spm, SSPM_SPM_RESUME))
		dev_warn(spm->dev, "SSPM did not take SPM_RESUME\n");
out_wdt:
	spm_gic_restore(spm);
	spm_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_PCM_WDT, 0, 0, 0);
	mt6765_mcdi_suspend_pause(false);

	return ret;
}

static const struct platform_suspend_ops mt6765_spm_sleep_ops = {
	.valid = suspend_valid_only_mem,
	.enter = mt6765_spm_sleep_enter,
};

static int mt6765_spm_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6765_spm_sleep *spm;
	struct device_node *np;
	struct resource *res;
	struct irq_data *irqd;
	long status;
	u32 idx;
	int ret;

	ret = of_property_read_u32(dev->of_node, "mediatek,spmfw-idx", &idx);
	if (ret)
		return dev_err_probe(dev, ret, "no mediatek,spmfw-idx\n");

	spm = devm_kzalloc(dev, sizeof(*spm), GFP_KERNEL);
	if (!spm)
		return -ENOMEM;
	spm->dev = dev;

	spm->mbox = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spm->mbox))
		return PTR_ERR(spm->mbox);
	spm->mbox_ctrl = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(spm->mbox_ctrl))
		return PTR_ERR(spm->mbox_ctrl);

	/* In the scpsys block, which the power domain driver has requested. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!res)
		return -EINVAL;
	spm->irq_sta = devm_ioremap(dev, res->start, resource_size(res));
	if (!spm->irq_sta)
		return -ENOMEM;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	/* The GIC INTID under sysirq, kept enabled around the sleep */
	irqd = irq_get_irq_data(ret);
	while (irqd->parent_data)
		irqd = irqd->parent_data;
	spm->irq = irqd->hwirq;
	ret = devm_request_irq(dev, ret, mt6765_spm_irq0, 0, dev_name(dev), spm);
	if (ret)
		return ret;

	np = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!np)
		return -ENODEV;
	/* The GIC driver owns the region; this only flips enable bits. */
	spm->gicd = of_iomap(np, 0);
	of_node_put(np);
	if (!spm->gicd)
		return -ENOMEM;
	spm->gicd_regs = GICD_TYPER_SPIS(readl(spm->gicd + GICD_TYPER)) / 32;
	spm->gicd_enable = devm_kcalloc(dev, spm->gicd_regs, sizeof(u32),
					GFP_KERNEL);
	if (!spm->gicd_enable)
		return -ENOMEM;

	status = spm_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0, 0);
	if (status < SPM_FIRMWARE_LOADED)
		return dev_err_probe(dev, -ENODEV,
				     "SPM firmware not loaded (%ld)\n", status);

	spm_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SPMFW_IDX, idx, 0, 0);
	spm->wake_src = spm_smc(MTK_SIP_SPM_GET_PWR_CTRL_ARGS,
				SPM_PWR_CTRL_SUSPEND, PW_WAKE_SRC, 0, 0);

	spm_sleep = spm;
	suspend_set_ops(&mt6765_spm_sleep_ops);
	dev_info(dev, "SPM firmware %u, status %ld, wake sources %#x\n",
		 idx, status, spm->wake_src);

	return 0;
}

static const struct of_device_id mt6765_spm_sleep_of_match[] = {
	{ .compatible = "mediatek,mt6765-spm-sleep" },
	{ }
};

static struct platform_driver mt6765_spm_sleep_driver = {
	.probe = mt6765_spm_sleep_probe,
	.driver = {
		.name = "mt6765-spm-sleep",
		.of_match_table = mt6765_spm_sleep_of_match,
		.suppress_bind_attrs = true,
	},
};

/* Select the firmware entry before the DVFSRC driver (device_initcall) kicks it. */
static int __init mt6765_spm_sleep_init(void)
{
	return platform_driver_register(&mt6765_spm_sleep_driver);
}
subsys_initcall(mt6765_spm_sleep_init);

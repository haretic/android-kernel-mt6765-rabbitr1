// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * MT6765 MFG (GE8320) platform glue for the PowerVR DDK on mainline Linux.
 *
 * The stock driver delegated power to MediaTek's gpufreq/GED frameworks:
 *   mt_gpufreq_voltage_enable_set(BUCK_ON)  -> VCORE PM-QoS vote
 *   mt_gpufreq_enable_MTCMOS(hwapm)         -> clk_mux on + parent MFGPLL,
 *                                              MFG_ASYNC, MFG (+ CORE0 when
 *                                              hardware APM is not used)
 *   MTKInitHWAPM()                          -> MFG_CFG APM programming
 * and the reverse on power off (mux back to 26 MHz).
 *
 * Here the same sequence is expressed with the common clock framework, the
 * generic power domain framework (SCPSYS MFG domains) and the regulator
 * framework. DVFS is not ported: the GPU runs at one fixed operating point.
 * The stock default (RGX_HW_CORE_CLOCK_SPEED) is 400 MHz, which is the lowest
 * entry of every MT6765 segment table and needs 0.65 V VCORE (VCORE_OPP_3).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include "mtk_mfgsys.h"
#include "rgxdevice.h"
#include "osfunc.h"
#include "pvrsrv.h"
#include "rgxhwperf.h"
#include "device.h"
#include "rgxinit.h"
#include "pvr_notifier.h"
#include "sysconfig.h"

#define mfg_readl(addr) readl(addr)
#define mfg_writel(val, addr) \
	do { writel(val, addr); wmb(); } while (0) /* sync_write */

/* MFG_CFG sits at GPU base + 0xffe000 (0x13ffe000) on MT6765. */
#define MTK_MFG_CFG_OFFSET	0xffe000
#define MTK_MFG_CFG_SIZE	0x1000

/* Lowest VCORE the stock table uses for the 400 MHz OPP (0.65 V). */
#define MTK_MFG_MIN_VCORE_UV	650000

static unsigned int mfg_freq_khz = RGX_HW_CORE_CLOCK_SPEED / 1000;
module_param(mfg_freq_khz, uint, 0444);
MODULE_PARM_DESC(mfg_freq_khz,
	"Fixed GPU core clock in kHz (default 400000, the stock MT6765 rate)");

static bool hwapm = true;
module_param(hwapm, bool, 0444);
MODULE_PARM_DESC(hwapm,
	"Use MFG hardware APM like the stock build (1) or keep MFG_CORE0 on from the host (0)");

static IMG_UINT32 gpu_power;
static IMG_UINT32 gpu_debug_enable;

module_param(gpu_power, uint, 0644);
module_param(gpu_debug_enable, uint, 0644);

struct mtk_mfg {
	struct device *dev;

	struct clk *clk_mux;		/* TOP_MFG_SEL */
	struct clk *clk_main_parent;	/* TOP_MFGPLL */
	struct clk *clk_sub_parent;	/* clk26m */

	struct regulator *vcore;	/* optional, shared VCORE */

	/* Virtual genpd devices when several domains are named in DT. */
	struct device *pd_mfg;
	struct device *pd_core0;
	bool own_pd_mfg;

	void __iomem *mfg_cfg;

	bool powered;
	bool vcore_warned;
};

static struct mtk_mfg *g_mfg;
static IMG_HANDLE g_RGXutilUser;

/*
 * Stock mt-plat (mtk_gpu_utility) owned these hooks for its GPU memory
 * reporting; process_stats.c still fills them in. Nothing reads them here.
 */
unsigned int (*mtk_get_gpu_memory_usage_fp)(void);
bool (*mtk_dump_gpu_memory_usage_fp)(void);

bool MTKMFGHwApmEnabled(void)
{
	return hwapm;
}

static PVRSRV_DEVICE_NODE *MTKGetRGXDevNode(void)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	IMG_UINT32 i;

	if (psPVRSRVData) {
		for (i = 0; i < psPVRSRVData->ui32RegisteredDevices; i++) {
			PVRSRV_DEVICE_NODE *psDeviceNode =
			&psPVRSRVData->psDeviceNodeList[i];

			if (psDeviceNode && psDeviceNode->psDevConfig)
				return psDeviceNode;
		}
	}
	return NULL;
}

static void MTKWriteBackFreqToRGX(PVRSRV_DEVICE_NODE *psDevNode,
		IMG_UINT32 ui32NewFreq)
{
	RGX_DATA *psRGXData = (RGX_DATA *)psDevNode->psDevConfig->hDevData;

	/* kHz to Hz write to RGX as the same unit */
	psRGXData->psRGXTimingInfo->ui32CoreClockSpeed = ui32NewFreq * 1000;
}

static void MTKInitHWAPM(struct mtk_mfg *mfg)
{
	void __iomem *base = mfg->mfg_cfg;

	if (!base)
		return;

	if (gpu_debug_enable)
		dev_info(mfg->dev, "HWAPM: LV0 0x01c = 0x%x\n",
			 mfg_readl(base + 0x01c));

	/* Values and order copied from the stock MTKInitHWAPM(). */
	mfg_writel(0x01a80000, (base + 0x504));
	mfg_writel(0x00080010, (base + 0x508));
	mfg_writel(0x00100008, (base + 0x50c));
	mfg_writel(0x00b800c8, (base + 0x510));
	mfg_writel(0x00b000c0, (base + 0x514));
	mfg_writel(0x00c000c8, (base + 0x518));
	mfg_writel(0x00b000b8, (base + 0x51c));
	mfg_writel(0x00d000d0, (base + 0x520));
	mfg_writel(0x00d000d0, (base + 0x524));
	mfg_writel(0x00d00000, (base + 0x528));

	mfg_writel(0x9004001b, (base + 0x24));
	mfg_writel(0x8004001b, (base + 0x24));

	if (gpu_debug_enable)
		dev_info(mfg->dev, "HWAPM: LV1 0x01c = 0x%x\n",
			 mfg_readl(base + 0x01c));
}

static int MTKEnableVcore(struct mtk_mfg *mfg)
{
	int ret, uv;

	if (!mfg->vcore)
		return 0;

	ret = regulator_enable(mfg->vcore);
	if (ret) {
		dev_err(mfg->dev, "failed to enable vcore: %d\n", ret);
		return ret;
	}

	/*
	 * VCORE is shared with DRAM and the rest of the SoC, so do not vote a
	 * voltage here (a min-only vote could pull it below what DRAM needs).
	 * Only check that the fixed OPP's requirement is met.
	 */
	uv = regulator_get_voltage(mfg->vcore);
	if (uv > 0 && uv < MTK_MFG_MIN_VCORE_UV && !mfg->vcore_warned) {
		dev_warn(mfg->dev, "vcore %d uV is below the %d uV the GPU OPP needs\n",
			 uv, MTK_MFG_MIN_VCORE_UV);
		mfg->vcore_warned = true;
	}

	return 0;
}

static void MTKDisableVcore(struct mtk_mfg *mfg)
{
	if (mfg->vcore)
		regulator_disable(mfg->vcore);
}

/* Mirrors mt_gpufreq_voltage_enable_set(BUCK_ON) + mt_gpufreq_enable_MTCMOS() */
static int MTKEnableMfgClock(struct mtk_mfg *mfg)
{
	int ret;

	if (mfg->powered)
		return 0;

	ret = MTKEnableVcore(mfg);
	if (ret)
		return ret;

	/* enable PLL and set clk_mux to mfgpll */
	ret = clk_prepare_enable(mfg->clk_mux);
	if (ret) {
		dev_err(mfg->dev, "failed to enable mfg mux: %d\n", ret);
		goto err_vcore;
	}

	ret = clk_set_parent(mfg->clk_mux, mfg->clk_main_parent);
	if (ret)
		dev_warn(mfg->dev, "failed to select mfgpll: %d\n", ret);

	/* MFG_ASYNC is the parent domain of MFG and is powered first. */
	ret = pm_runtime_resume_and_get(mfg->pd_mfg);
	if (ret) {
		dev_err(mfg->dev, "failed to power on MFG: %d\n", ret);
		goto err_mux;
	}

	/* With hardware APM the MFG block powers CORE0 itself. */
	if (!hwapm && mfg->pd_core0) {
		ret = pm_runtime_resume_and_get(mfg->pd_core0);
		if (ret) {
			dev_err(mfg->dev, "failed to power on MFG_CORE0: %d\n",
				ret);
			goto err_mfg;
		}
	}

	mfg->powered = true;

	if (gpu_debug_enable)
		dev_info(mfg->dev, "%s\n", __func__);

	return 0;

err_mfg:
	pm_runtime_put_sync(mfg->pd_mfg);
err_mux:
	clk_set_parent(mfg->clk_mux, mfg->clk_sub_parent);
	clk_disable_unprepare(mfg->clk_mux);
err_vcore:
	MTKDisableVcore(mfg);
	return ret;
}

/* Mirrors mt_gpufreq_disable_MTCMOS() + mt_gpufreq_voltage_enable_set(BUCK_OFF) */
static void MTKDisableMfgClock(struct mtk_mfg *mfg)
{
	if (!mfg->powered)
		return;

	if (!hwapm && mfg->pd_core0)
		pm_runtime_put_sync(mfg->pd_core0);

	pm_runtime_put_sync(mfg->pd_mfg);

	/* set clk_mux to 26M and turn-off gpupll reference */
	clk_set_parent(mfg->clk_mux, mfg->clk_sub_parent);
	clk_disable_unprepare(mfg->clk_mux);

	MTKDisableVcore(mfg);

	mfg->powered = false;

	if (gpu_debug_enable)
		dev_info(mfg->dev, "%s\n", __func__);
}

PVRSRV_ERROR MTKDevPrePowerState(IMG_HANDLE hSysData,
	PVRSRV_DEV_POWER_STATE eNewPowerState,
	PVRSRV_DEV_POWER_STATE eCurrentPowerState,
	IMG_BOOL bForced)
{
	if (!g_mfg)
		return PVRSRV_OK;

	if (eNewPowerState == PVRSRV_DEV_POWER_STATE_OFF &&
		eCurrentPowerState == PVRSRV_DEV_POWER_STATE_ON)
		MTKDisableMfgClock(g_mfg);

	return PVRSRV_OK;
}

PVRSRV_ERROR MTKDevPostPowerState(IMG_HANDLE hSysData,
	PVRSRV_DEV_POWER_STATE eNewPowerState,
	PVRSRV_DEV_POWER_STATE eCurrentPowerState,
	IMG_BOOL bForced)
{
	if (!g_mfg)
		return PVRSRV_OK;

	if (eCurrentPowerState == PVRSRV_DEV_POWER_STATE_OFF &&
		eNewPowerState == PVRSRV_DEV_POWER_STATE_ON) {
		if (MTKEnableMfgClock(g_mfg))
			return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
		if (hwapm)
			MTKInitHWAPM(g_mfg);
	}

	return PVRSRV_OK;
}

PVRSRV_ERROR MTKSystemPrePowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	return PVRSRV_OK;
}

PVRSRV_ERROR MTKSystemPostPowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	return PVRSRV_OK;
}

void mtk_fdvfs_update_cur_freq(int ui32GPUFreq)
{
	PVRSRV_DEVICE_NODE *psDevNode = MTKGetRGXDevNode();
	PVRSRV_ERROR eResult;
	static int s_ui32GPUFreq;

	if (s_ui32GPUFreq != ui32GPUFreq) {
		if (psDevNode) {
			eResult = PVRSRVDevicePreClockSpeedChange(psDevNode,
			IMG_FALSE, (void *)NULL);
			if ((eResult == PVRSRV_OK) ||
				(eResult == PVRSRV_ERROR_RETRY)) {
				MTKWriteBackFreqToRGX(psDevNode, ui32GPUFreq);
				if (eResult == PVRSRV_OK)
					PVRSRVDevicePostClockSpeedChange(psDevNode,
					IMG_FALSE, (void *)NULL);
			}
		}
		s_ui32GPUFreq = ui32GPUFreq;
	}
}
EXPORT_SYMBOL(mtk_fdvfs_update_cur_freq);

PVRSRV_ERROR MTKMFGSystemInit(void)
{
	return PVRSRV_OK;
}

void MTKMFGSystemDeInit(void)
{
}

static void MTKDetachPowerDomains(struct mtk_mfg *mfg)
{
	if (mfg->pd_core0) {
		dev_pm_domain_detach(mfg->pd_core0, true);
		mfg->pd_core0 = NULL;
	}
	if (mfg->own_pd_mfg && mfg->pd_mfg)
		dev_pm_domain_detach(mfg->pd_mfg, true);
	else if (mfg->pd_mfg)
		pm_runtime_disable(mfg->pd_mfg);
	mfg->pd_mfg = NULL;
}

/*
 * DT may either reference only MFG (the driver core then attaches it to the
 * GPU device) or name "mfg" and "core0" so that CORE0 can be host-controlled
 * when hardware APM is disabled.
 */
static int MTKAttachPowerDomains(struct mtk_mfg *mfg)
{
	struct device *dev = mfg->dev;
	struct device *pd;

	if (of_count_phandle_with_args(dev->of_node, "power-domains",
				       "#power-domain-cells") <= 1) {
		if (!dev->pm_domain) {
			dev_err(dev, "no MFG power domain\n");
			return -ENODEV;
		}
		pm_runtime_enable(dev);
		mfg->pd_mfg = dev;
		return 0;
	}

	pd = dev_pm_domain_attach_by_name(dev, "mfg");
	if (IS_ERR_OR_NULL(pd)) {
		dev_err(dev, "failed to attach the mfg power domain\n");
		return pd ? PTR_ERR(pd) : -ENODEV;
	}
	mfg->pd_mfg = pd;
	mfg->own_pd_mfg = true;

	pd = dev_pm_domain_attach_by_name(dev, "core0");
	if (IS_ERR(pd)) {
		MTKDetachPowerDomains(mfg);
		return PTR_ERR(pd);
	}
	mfg->pd_core0 = pd;

	return 0;
}

static int MTKGetClocks(struct mtk_mfg *mfg)
{
	struct device *dev = mfg->dev;

	mfg->clk_mux = devm_clk_get(dev, "mux");
	if (IS_ERR(mfg->clk_mux))
		return dev_err_probe(dev, PTR_ERR(mfg->clk_mux),
				     "cannot get mfg mux clock\n");

	mfg->clk_main_parent = devm_clk_get(dev, "main_parent");
	if (IS_ERR(mfg->clk_main_parent))
		return dev_err_probe(dev, PTR_ERR(mfg->clk_main_parent),
				     "cannot get mfgpll clock\n");

	mfg->clk_sub_parent = devm_clk_get(dev, "sub_parent");
	if (IS_ERR(mfg->clk_sub_parent))
		return dev_err_probe(dev, PTR_ERR(mfg->clk_sub_parent),
				     "cannot get 26 MHz clock\n");

	return 0;
}

static void MTKSetFixedFreq(struct mtk_mfg *mfg, PVRSRV_DEVICE_CONFIG *psDevConfig)
{
	RGX_DATA *psRGXData = (RGX_DATA *)psDevConfig->hDevData;
	unsigned long rate;
	int ret;

	ret = clk_set_rate(mfg->clk_main_parent, mfg_freq_khz * 1000UL);
	if (ret)
		dev_warn(mfg->dev, "failed to set mfgpll to %u kHz: %d\n",
			 mfg_freq_khz, ret);

	rate = clk_get_rate(mfg->clk_main_parent);
	dev_info(mfg->dev, "fixed GPU clock %lu Hz (requested %u kHz), hwapm=%d\n",
		 rate, mfg_freq_khz, hwapm);

	if (rate && psRGXData && psRGXData->psRGXTimingInfo)
		psRGXData->psRGXTimingInfo->ui32CoreClockSpeed = rate;
}

int MTKRGXDeviceInit(PVRSRV_DEVICE_CONFIG *psDevConfig)
{
	struct device *dev = psDevConfig->pvOSDevice;
	struct platform_device *pdev = to_platform_device(dev);
	struct mtk_mfg *mfg;
	int ret;

	mfg = devm_kzalloc(dev, sizeof(*mfg), GFP_KERNEL);
	if (!mfg)
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	mfg->dev = dev;

	ret = MTKGetClocks(mfg);
	if (ret)
		return ret == -EPROBE_DEFER ? PVRSRV_ERROR_PROBE_DEFER :
					      PVRSRV_ERROR_INIT_FAILURE;

	mfg->vcore = devm_regulator_get_optional(dev, "vcore");
	if (IS_ERR(mfg->vcore)) {
		ret = PTR_ERR(mfg->vcore);
		if (ret == -EPROBE_DEFER)
			return PVRSRV_ERROR_PROBE_DEFER;
		if (ret != -ENODEV) {
			dev_err(dev, "cannot get vcore: %d\n", ret);
			return PVRSRV_ERROR_INIT_FAILURE;
		}
		mfg->vcore = NULL;
	}

	mfg->mfg_cfg = devm_platform_ioremap_resource_byname(pdev, "mfg_cfg");
	if (IS_ERR(mfg->mfg_cfg)) {
		/* Stock derives MFG_CFG from the GPU register base. */
		mfg->mfg_cfg = devm_ioremap(dev,
			psDevConfig->sRegsCpuPBase.uiAddr + MTK_MFG_CFG_OFFSET,
			MTK_MFG_CFG_SIZE);
		if (!mfg->mfg_cfg) {
			dev_err(dev, "cannot map MFG_CFG\n");
			return PVRSRV_ERROR_INIT_FAILURE;
		}
	}

	ret = MTKAttachPowerDomains(mfg);
	if (ret)
		return ret == -EPROBE_DEFER ? PVRSRV_ERROR_PROBE_DEFER :
					      PVRSRV_ERROR_INIT_FAILURE;

	MTKSetFixedFreq(mfg, psDevConfig);

	g_mfg = mfg;

#ifndef MTK_BRINGUP
	/* Enable CG & mtcmos once at init, as the stock driver does. */
	ret = MTKEnableMfgClock(mfg);
	if (ret) {
		g_mfg = NULL;
		MTKDetachPowerDomains(mfg);
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	if (hwapm)
		MTKInitHWAPM(mfg);
#endif

	/* 1.5+DDK support multiple user to query GPU utilization */
	/* Need Init here */
	if (g_RGXutilUser == NULL)
		SORgxGpuUtilStatsRegister(&g_RGXutilUser);

#if MTK_PM_SUPPORT
	MTKDisableMfgClock(mfg);
#endif

	return 0;
}

int MTKRGXDeviceDeInit(PVRSRV_DEVICE_CONFIG *psDevConfig)
{
	struct mtk_mfg *mfg = g_mfg;

	if (g_RGXutilUser) {
		SORgxGpuUtilStatsUnregister(g_RGXutilUser);
		g_RGXutilUser = NULL;
	}

	if (!mfg)
		return 0;

	MTKDisableMfgClock(mfg);
	MTKDetachPowerDomains(mfg);
	g_mfg = NULL;

	return 0;
}

void MTKFWDump(void)
{
	PVRSRV_DEVICE_NODE *psDevNode = MTKGetRGXDevNode();

	if (psDevNode) {
		MTK_PVRSRVDebugRequestSetSilence(IMG_TRUE);
		PVRSRVDebugRequest(psDevNode, DEBUG_REQUEST_VERBOSITY_MAX, NULL, NULL);
		MTK_PVRSRVDebugRequestSetSilence(IMG_FALSE);
	}
}
EXPORT_SYMBOL(MTKFWDump);

int mtk_mfg_async_init(void)
{
	return 0;
}

int mtk_mfg_2d_init(void)
{
	return 0;
}

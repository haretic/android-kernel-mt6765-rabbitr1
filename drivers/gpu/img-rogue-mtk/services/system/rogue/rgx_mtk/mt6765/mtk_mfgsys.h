/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * MT6765 MFG platform glue for the PowerVR DDK on mainline Linux. The stock
 * version of this header pulled in GED, gpufreq and SSPM hooks; those
 * frameworks are not ported, so only the interface the DDK core uses is kept.
 */

#ifndef MTK_MFGSYS_H
#define MTK_MFGSYS_H

#include "servicesext.h"
#include "rgxdevice.h"

/* Control SW APM is enabled or not  */
#ifndef MTK_BRINGUP
#define MTK_PM_SUPPORT 1
#define MTCMOS_CONTROL 1
#else
#define MTK_PM_SUPPORT 0
#endif

void mtk_fdvfs_update_cur_freq(int ui32GPUFreq);
PVRSRV_ERROR MTKMFGSystemInit(void);
void MTKMFGSystemDeInit(void);
void MTKFWDump(void);

/* below register interface in RGX sysconfig.c */
PVRSRV_ERROR MTKDevPrePowerState(IMG_HANDLE hSysData,
				PVRSRV_DEV_POWER_STATE eNewPowerState,
				PVRSRV_DEV_POWER_STATE eCurrentPowerState,
				IMG_BOOL bForced);

PVRSRV_ERROR MTKDevPostPowerState(IMG_HANDLE hSysData,
				PVRSRV_DEV_POWER_STATE eNewPowerState,
				PVRSRV_DEV_POWER_STATE eCurrentPowerState,
				IMG_BOOL bForced);

PVRSRV_ERROR MTKSystemPrePowerState(PVRSRV_SYS_POWER_STATE eNewPowerState);

PVRSRV_ERROR MTKSystemPostPowerState(PVRSRV_SYS_POWER_STATE eNewPowerState);

int MTKRGXDeviceInit(PVRSRV_DEVICE_CONFIG *psDevConfig);
int MTKRGXDeviceDeInit(PVRSRV_DEVICE_CONFIG *psDevConfig);

/* True when the MFG hardware APM owns MFG_CORE0 (stock MTK_USE_HW_APM). */
bool MTKMFGHwApmEnabled(void);

int mtk_mfg_async_init(void);
int mtk_mfg_2d_init(void);

#endif

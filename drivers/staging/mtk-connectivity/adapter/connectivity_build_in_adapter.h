/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 *
 * Mainline replacement for the vendor kernel's connectivity build-in
 * adapter. On the stock kernel this file mapped KERNEL_* names onto
 * symbols that the vendor kernel exported for the out-of-tree modules
 * (clock buffer, legacy PMIC wrapper, MT6306 GPIO expander, CPU boost,
 * MMC internals). None of those exist upstream.
 *
 * On MT6765 the WMT platform code takes the "common kernel" path
 * (COMMON_KERNEL_PMIC_SUPPORT/COMMON_KERNEL_CLK_SUPPORT), so the clock
 * buffer and legacy PMIC hooks are not referenced; leaving them undefined
 * makes any new user fail to build instead of silently doing nothing.
 */

#ifndef CONNECTIVITY_BUILD_IN_ADAPTER_H
#define CONNECTIVITY_BUILD_IN_ADAPTER_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/sched/clock.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/notifier.h>

/*******************************************************************************
 * MMC (only used by the SDIO combo-chip path, which MT6765 never takes)
 ******************************************************************************/
#define KERNEL_mmc_io_rw_direct connectivity_export_mmc_io_rw_direct
struct mmc_card;
int connectivity_export_mmc_io_rw_direct(struct mmc_card *card, int write,
						unsigned int fn,
						unsigned int addr,
						u8 in, u8 *out);

/*******************************************************************************
 * Debug helpers
 ******************************************************************************/
#define KERNEL_mt_ppm_sysboost_freq
#define KERNEL_mt_ppm_sysboost_core
#define KERNEL_mt_ppm_sysboost_set_core_limit
#define KERNEL_mt_ppm_sysboost_set_freq_limit
#define KERNEL_spm_resource_req

#define KERNEL_show_stack connectivity_export_show_stack
#define KERNEL_dump_thread_state connectivity_export_dump_thread_state

void connectivity_export_show_stack(struct task_struct *tsk, unsigned long *sp);
void connectivity_export_dump_thread_state(const char *name);

struct connsys_state_info {
	unsigned int chip_info;
	phys_addr_t emi_phy_addr;
};
void connectivity_export_conap_scp_init(unsigned int chip_info, phys_addr_t emi_phy_addr);
void connectivity_export_conap_scp_deinit(void);

#endif /* CONNECTIVITY_BUILD_IN_ADAPTER_H */

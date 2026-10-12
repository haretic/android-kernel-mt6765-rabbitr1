// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 *
 * Mainline replacement for the vendor kernel's connectivity build-in
 * adapter, linked into wmt_drv.ko. See connectivity_build_in_adapter.h.
 */

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[CONNADP]"
#include "connectivity_build_in_adapter.h"

#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>

void connectivity_export_show_stack(struct task_struct *tsk, unsigned long *sp)
{
	if (tsk)
		sched_show_task(tsk);
}

void connectivity_export_conap_scp_init(unsigned int chip_info, phys_addr_t emi_phy_addr)
{
}

void connectivity_export_conap_scp_deinit(void)
{
}

/*******************************************************************************
 * MMC
 ******************************************************************************/
int connectivity_export_mmc_io_rw_direct(struct mmc_card *card,
				int write, unsigned int fn,
				unsigned int addr, u8 in, u8 *out)
{
	/* mmc_io_rw_direct() is internal to the MMC core upstream. */
	return -EOPNOTSUPP;
}

void connectivity_export_dump_thread_state(const char *name)
{
	struct task_struct *p;

	if (name == NULL || strlen(name) > 255) {
		pr_info(DFT_TAG "invalid name:%p or thread name too long\n", name);
		return;
	}

	pr_info(DFT_TAG "start to show debug info of %s\n", name);

	rcu_read_lock();
	for_each_process(p) {
		if (strncmp(p->comm, name, strlen(name)) != 0)
			continue;
		sched_show_task(p);
		break;
	}
	rcu_read_unlock();
}

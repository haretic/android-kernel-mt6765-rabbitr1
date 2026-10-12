/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The MediaTek clock buffer driver is not available. On the MT6765 the
 * modem only used it to switch the PMIC baseband clock buffer low power
 * mode with flight mode, which stays in its boot default here.
 */
#ifndef __MTK_CCCI_CLKBUF_CTL_H__
#define __MTK_CCCI_CLKBUF_CTL_H__

#include <linux/types.h>

static inline void clk_buf_set_by_flightmode(bool is_flightmode_on)
{
}

bool is_clk_buf_from_pmic(void);
void clk_buf_get_swctrl_status(void *swctrl_status);
void clk_buf_get_rf_drv_curr(void *rf_drv_curr);
void clk_buf_save_afc_val(unsigned int afcdac);

#endif

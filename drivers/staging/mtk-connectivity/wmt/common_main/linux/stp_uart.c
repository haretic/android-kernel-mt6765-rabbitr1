// SPDX-License-Identifier: GPL-2.0
/*
 * The stock stp_uart.c registers the "n_mtkstp" TTY line discipline that
 * carries STP to an external MT66xx combo chip over a UART. The MT6765
 * CONSYS talks STP over BTIF (stp_btif.c), so the line discipline is not
 * built here; these entry points stay so the core does not need to change.
 */

#include "osal_typedef.h"

INT32 mtk_wcn_stp_uart_drv_init(VOID)
{
	return 0;
}

VOID mtk_wcn_stp_uart_drv_exit(VOID)
{
}

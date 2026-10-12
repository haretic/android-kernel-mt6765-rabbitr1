/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 - 2021 MediaTek Inc.
 */


#ifndef _GPS_H_
#define _GPS_H_

#include "wmt_core.h"
#include "wmt_dev.h"
#include "osal.h"
#include "mtk_wcn_consys_hw.h"

/*
 * Only the MT6765 CONSYS is built here. Its firmware takes the GPS FWCTL
 * opcodes (stock enabled this through CONFIG_MACH_MT6765).
 */
#define GPS_FWCTL_SUPPORT

#ifdef GPS_FWCTL_SUPPORT
/* Disable: #define GPS_FWCTL_IOCTL_SUPPORT */

#define GPS_HW_SUSPEND_SUPPORT
#endif /* GPS_FWCTL_SUPPORT */

enum gps_ctrl_status_enum {
	GPS_CLOSED,
	GPS_OPENED,
	GPS_SUSPENDED,
	GPS_RESET_START,
	GPS_RESET_DONE,
	GPS_CTRL_STATUS_NUM
};

enum gps_reference_count_cmd {
	FWLOG_CTRL_INNER = 0,
	HANDLE_DESENSE,
	GPS_FWCTL_READY,
};

#ifdef GPS_FWCTL_SUPPORT
#define GPS_FWCTL_OPCODE_ENTER_SLEEP_MODE (2)
#define GPS_FWCTL_OPCODE_EXIT_SLEEP_MODE  (3)
#define GPS_FWCTL_OPCODE_ENTER_STOP_MODE  (4)
#define GPS_FWCTL_OPCODE_EXIT_STOP_MODE   (5)
#define GPS_FWCTL_OPCODE_LOG_CFG          (6)
#define GPS_FWCTL_OPCODE_LOOPBACK_TEST    (7)
#define GPS2_FWCTL_OPCODE_ENTER_SLEEP_MODE (0x12)
#define GPS2_FWCTL_OPCODE_EXIT_SLEEP_MODE  (0x13)
#define GPS2_FWCTL_OPCODE_ENTER_STOP_MODE (0x14)
#define GPS2_FWCTL_OPCODE_EXIT_STOP_MODE  (0x15)

#endif

enum gps_data_link_id_enum {
	GPS_DATA_LINK_ID0	= 0,
	GPS_DATA_LINK_ID1	= 1,
	GPS_DATA_LINK_NUM	= 2,
};
extern int gps_stp_get_md_status(struct device *dev);
extern void GPS_reference_count(enum gps_reference_count_cmd cmd, bool flag, enum gps_data_link_id_enum user);

extern phys_addr_t gConEmiPhyBase;

extern int mtk_wcn_stpgps_drv_init(void);
extern void mtk_wcn_stpgps_drv_exit(void);
extern int mtk_gps_emi_init(void);
extern void mtk_gps_emi_exit(void);
#ifdef CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH
extern int mtk_gps_fw_log_init(void);
extern void mtk_gps_fw_log_exit(void);
void GPS_fwlog_ctrl(bool on);
#endif

extern struct semaphore fwctl_mtx;

#ifdef CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH
extern bool fgGps_fwlog_on;
#endif

#ifdef GPS_FWCTL_SUPPORT
extern bool fgGps_fwctl_ready;
#endif

#endif

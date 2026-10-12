/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_CCCI_MAIN_H__
#define __MTK_CCCI_MAIN_H__

#include <linux/init.h>

struct platform_device;

int __init ccci_util_init(void);
int __init ccci_init(void);
int __init ccci_auxadc_init(void);
int __init modem_cd_init(void);

int ccci_ccif_hif_init(struct platform_device *pdev,
	unsigned char hif_id, unsigned char md_id);
int ccci_cldma_hif_init(struct platform_device *pdev,
	unsigned char hif_id, unsigned char md_id);

#endif /* __MTK_CCCI_MAIN_H__ */

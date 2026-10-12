/* SPDX-License-Identifier: GPL-2.0 */
/*
 * In-kernel interface of the legacy MediaTek ION compatibility device.
 */

#ifndef _MTK_ION_COMPAT_H
#define _MTK_ION_COMPAT_H

struct device;

/*
 * Serve the device addresses of the stock M4U ports first_port..last_port
 * (the module ids the blobs pass to ION_MM_CONFIG_BUFFER, numbered as in the
 * stock kernel's m4u_port.h, not as in dt-bindings) by mapping buffers
 * through @dev. The mappings are removed when @dev is unbound.
 */
int devm_mtk_ion_compat_add_iova_device(struct device *dev, int first_port,
					int last_port);

#endif /* _MTK_ION_COMPAT_H */

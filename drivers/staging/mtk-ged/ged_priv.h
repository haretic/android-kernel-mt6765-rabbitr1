/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek GED compatibility driver, internal interfaces.
 */

#ifndef _MTK_GED_PRIV_H
#define _MTK_GED_PRIV_H

#include "ged_abi.h"

int ged_bridge_ge_alloc(struct GED_BRIDGE_IN_GE_ALLOC *in,
			struct GED_BRIDGE_OUT_GE_ALLOC *out);
int ged_bridge_ge_get(struct GED_BRIDGE_IN_GE_GET *in,
		      struct GED_BRIDGE_OUT_GE_GET *out, int out_size);
int ged_bridge_ge_set(struct GED_BRIDGE_IN_GE_SET *in,
		      struct GED_BRIDGE_OUT_GE_SET *out, int in_size);
int ged_bridge_ge_info(struct GED_BRIDGE_IN_GE_INFO *in,
		       struct GED_BRIDGE_OUT_GE_INFO *out);

#endif /* _MTK_GED_PRIV_H */

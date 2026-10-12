/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shims for building MediaTek's out-of-tree connectivity
 * drivers (written against 4.19/5.10 GKI) on this kernel. Force-included
 * into every object of wmt_drv, wmt_chrdev_wifi and wlan_drv_gen4m.
 *
 * Only add what cannot reasonably be fixed at the call site.
 */
#ifndef _MTK_CONN_COMPAT_H
#define _MTK_CONN_COMPAT_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timer.h>

/* strlcpy() was removed in 6.8; nothing here uses its return value. */
#ifndef strlcpy
#define strlcpy(d, s, n)	strscpy((d), (s), (n))
#endif

/* Timer API renames (6.15/6.16). */
#ifndef del_timer
#define del_timer(t)		timer_delete(t)
#endif
#ifndef del_timer_sync
#define del_timer_sync(t)	timer_delete_sync(t)
#endif
#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#endif

/*
 * of_get_named_gpio() is gone. The stock code only uses it for external
 * combo-chip pins and the Wi-Fi antenna swap GPIO, neither of which the
 * MT6765 CONSYS on this board has; refuse loudly if a DT ever sets one.
 */
#include <linux/of.h>
#include <linux/errno.h>
#include <linux/printk.h>
static inline int mtk_conn_of_get_named_gpio(const struct device_node *np,
					     const char *propname, int index)
{
	if (np && of_property_present(np, propname))
		pr_err("mtk-connectivity: %pOF: legacy GPIO property %s is not supported\n",
		       np, propname);
	return -ENOENT;
}
#define of_get_named_gpio(np, propname, index) \
	mtk_conn_of_get_named_gpio(np, propname, index)

#endif /* _MTK_CONN_COMPAT_H */

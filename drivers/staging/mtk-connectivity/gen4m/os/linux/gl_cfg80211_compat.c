// SPDX-License-Identifier: GPL-2.0
/*
 * Mainline port: adapters from the current cfg80211_ops prototypes to the
 * gen4m mtk_cfg_* dispatch wrappers (see gl_cfg80211_compat.h).
 *
 * A wireless_dev without a net_device (P2P device, NAN) never had these
 * ops on gen4m; refuse it instead of passing a NULL net_device down.
 */

#include "gl_os.h"
#include "precomp.h"
#include <net/cfg80211.h>
#include "gl_cfg80211.h"

#if (CFG_ENABLE_UNIFY_WIPHY == 1)

#define MTK_CFG_GET_NDEV(_ndev, _dev)					\
	do {								\
		(_ndev) = MTK_CFG_NDEV(_dev);				\
		if (!(_ndev))						\
			return -EOPNOTSUPP;				\
	} while (0)

#if KERNEL_VERSION(6, 1, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_add_key(struct wiphy *wiphy, MTK_CFG_STA_DEV_T dev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr, struct key_params *params)
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, dev);
	return mtk_cfg_add_key(wiphy, ndev, key_index, pairwise, mac_addr,
			       params);
}

int mtk_cfg_op_get_key(struct wiphy *wiphy, MTK_CFG_STA_DEV_T dev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr, void *cookie,
		       void (*callback)(void *cookie, struct key_params *))
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, dev);
	return mtk_cfg_get_key(wiphy, ndev, key_index, pairwise, mac_addr,
			       cookie, callback);
}

int mtk_cfg_op_del_key(struct wiphy *wiphy, MTK_CFG_STA_DEV_T dev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr)
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, dev);
	return mtk_cfg_del_key(wiphy, ndev, key_index, pairwise, mac_addr);
}

int mtk_cfg_op_set_default_key(struct wiphy *wiphy, struct net_device *ndev,
			       int link_id, u8 key_index, bool unicast,
			       bool multicast)
{
	return mtk_cfg_set_default_key(wiphy, ndev, key_index, unicast,
				       multicast);
}

int mtk_cfg_op_set_default_mgmt_key(struct wiphy *wiphy,
				    MTK_CFG_STA_DEV_T dev, int link_id,
				    u8 key_index)
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, dev);
	return mtk_cfg_set_default_mgmt_key(wiphy, ndev, key_index);
}
#endif

#if MTK_CFG80211_WDEV_STA_OPS <= CFG80211_VERSION_CODE
int mtk_cfg_op_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   const u8 *mac, struct station_info *sinfo)
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, wdev);
	return mtk_cfg_get_station(wiphy, ndev, mac, sinfo);
}

#if CFG_SUPPORT_TDLS
int mtk_cfg_op_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			      const u8 *mac,
			      struct station_parameters *params)
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, wdev);
	return mtk_cfg_change_station(wiphy, ndev, mac, params);
}

int mtk_cfg_op_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   const u8 *mac, struct station_parameters *params)
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, wdev);
	return mtk_cfg_add_station(wiphy, ndev, mac, params);
}
#endif

int mtk_cfg_op_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   struct station_del_parameters *params)
{
	struct net_device *ndev;

	MTK_CFG_GET_NDEV(ndev, wdev);
	return mtk_cfg_del_station(wiphy, ndev, params);
}
#endif

#if CFG_SUPPORT_TDLS && (KERNEL_VERSION(6, 5, 0) <= CFG80211_VERSION_CODE)
int mtk_cfg_op_tdls_mgmt(struct wiphy *wiphy, struct net_device *dev,
			 const u8 *peer, int link_id, u8 action_code,
			 u8 dialog_token, u16 status_code,
			 u32 peer_capability, bool initiator,
			 const u8 *buf, size_t len)
{
	return mtk_cfg_tdls_mgmt(wiphy, dev, peer, action_code, dialog_token,
				 status_code, peer_capability, initiator,
				 buf, len);
}
#endif

#if (CFG_SUPPORT_DFS_MASTER == 1) && \
	(KERNEL_VERSION(6, 12, 0) <= CFG80211_VERSION_CODE)
int mtk_cfg_op_start_radar_detection(struct wiphy *wiphy,
				     struct net_device *dev,
				     struct cfg80211_chan_def *chandef,
				     u32 cac_time_ms, int link_id)
{
	return mtk_cfg_start_radar_detection(wiphy, dev, chandef,
					     cac_time_ms);
}
#endif

#if (CFG_ENABLE_WIFI_DIRECT_CFG_80211 != 0)
#if KERNEL_VERSION(6, 7, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_change_beacon(struct wiphy *wiphy, struct net_device *dev,
			     struct cfg80211_ap_update *info)
{
	return mtk_cfg_change_beacon(wiphy, dev, &info->beacon);
}
#endif

#if KERNEL_VERSION(6, 0, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_stop_ap(struct wiphy *wiphy, struct net_device *dev,
		       unsigned int link_id)
{
	return mtk_cfg_stop_ap(wiphy, dev);
}

int mtk_cfg_op_set_bitrate_mask(struct wiphy *wiphy, struct net_device *dev,
				unsigned int link_id, const u8 *peer,
				const struct cfg80211_bitrate_mask *mask)
{
	return mtk_cfg_set_bitrate_mask(wiphy, dev, peer, mask);
}
#endif

#if KERNEL_VERSION(6, 16, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_set_wiphy_params(struct wiphy *wiphy, int radio_idx,
				u32 changed)
{
	return mtk_cfg_set_wiphy_params(wiphy, changed);
}

int mtk_cfg_op_set_txpower(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int radio_idx, enum nl80211_tx_power_setting type,
			   int mbm)
{
	return mtk_cfg_set_txpower(wiphy, wdev, type, mbm);
}

int mtk_cfg_op_get_txpower(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int radio_idx, unsigned int link_id, int *dbm)
{
	return mtk_cfg_get_txpower(wiphy, wdev, dbm);
}
#elif KERNEL_VERSION(6, 14, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_get_txpower(struct wiphy *wiphy, struct wireless_dev *wdev,
			   unsigned int link_id, int *dbm)
{
	return mtk_cfg_get_txpower(wiphy, wdev, dbm);
}
#endif
#endif /* (CFG_ENABLE_WIFI_DIRECT_CFG_80211 != 0) */

#endif /* (CFG_ENABLE_UNIFY_WIPHY == 1) */

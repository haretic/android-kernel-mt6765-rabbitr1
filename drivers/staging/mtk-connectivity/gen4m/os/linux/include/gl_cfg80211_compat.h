/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mainline port: cfg80211 changes after the 5.15 GKI that gen4m was last
 * updated for. The mtk_cfg_* dispatch wrappers keep their old prototypes;
 * gl_cfg80211_compat.c adapts the current cfg80211_ops prototypes to them
 * and mtk_cfg_ops refers to the mtk_cfg_op_* names below, which are the
 * plain wrappers on kernels that need no adapter.
 *
 * The CONNAC1 firmware has no MLO and the wiphy has a single radio, so
 * link ids and radio indices are ignored.
 */
#ifndef _GL_CFG80211_COMPAT_H
#define _GL_CFG80211_COMPAT_H

/*
 * The key and station ops, cfg80211_new_sta() and cfg80211_del_sta()
 * take a struct wireless_dev instead of a struct net_device since the
 * NAN station rework. 7.1 has it; the first release was not verified.
 */
#define MTK_CFG80211_WDEV_STA_OPS	KERNEL_VERSION(7, 0, 0)

/* the struct net_device a wireless_dev or net_device argument refers to */
#define MTK_CFG_NDEV(dev)						\
	_Generic((dev),							\
		 struct wireless_dev *:					\
			((struct wireless_dev *)(dev))->netdev,		\
		 struct net_device *: (dev))

/* what cfg80211_new_sta()/cfg80211_del_sta() take for a net_device */
#if MTK_CFG80211_WDEV_STA_OPS <= CFG80211_VERSION_CODE
#define MTK_CFG80211_STA_DEV(ndev)	((ndev)->ieee80211_ptr)
#else
#define MTK_CFG80211_STA_DEV(ndev)	(ndev)
#endif

/* station_parameters: the per-link fields moved to link_sta_params (6.0) */
#if KERNEL_VERSION(6, 0, 0) <= CFG80211_VERSION_CODE
#define MTK_STA_LINK_PARAMS(p)		(&(p)->link_sta_params)
#else
#define MTK_STA_LINK_PARAMS(p)		(p)
#endif

#if MTK_CFG80211_WDEV_STA_OPS <= CFG80211_VERSION_CODE
#define MTK_CFG_STA_DEV_T		struct wireless_dev *
#else
#define MTK_CFG_STA_DEV_T		struct net_device *
#endif

/* key ops: link_id (6.1), then wireless_dev for all but set_default_key */
#if KERNEL_VERSION(6, 1, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_add_key(struct wiphy *wiphy, MTK_CFG_STA_DEV_T dev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr, struct key_params *params);
int mtk_cfg_op_get_key(struct wiphy *wiphy, MTK_CFG_STA_DEV_T dev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr, void *cookie,
		       void (*callback)(void *cookie, struct key_params *));
int mtk_cfg_op_del_key(struct wiphy *wiphy, MTK_CFG_STA_DEV_T dev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr);
int mtk_cfg_op_set_default_key(struct wiphy *wiphy, struct net_device *ndev,
			       int link_id, u8 key_index, bool unicast,
			       bool multicast);
int mtk_cfg_op_set_default_mgmt_key(struct wiphy *wiphy,
				    MTK_CFG_STA_DEV_T dev, int link_id,
				    u8 key_index);
#else
#define mtk_cfg_op_add_key		mtk_cfg_add_key
#define mtk_cfg_op_get_key		mtk_cfg_get_key
#define mtk_cfg_op_del_key		mtk_cfg_del_key
#define mtk_cfg_op_set_default_key	mtk_cfg_set_default_key
#define mtk_cfg_op_set_default_mgmt_key	mtk_cfg_set_default_mgmt_key
#endif

/* station ops: wireless_dev */
#if MTK_CFG80211_WDEV_STA_OPS <= CFG80211_VERSION_CODE
int mtk_cfg_op_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   const u8 *mac, struct station_info *sinfo);
int mtk_cfg_op_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			      const u8 *mac,
			      struct station_parameters *params);
int mtk_cfg_op_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   const u8 *mac, struct station_parameters *params);
int mtk_cfg_op_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   struct station_del_parameters *params);
#else
#define mtk_cfg_op_get_station		mtk_cfg_get_station
#define mtk_cfg_op_change_station	mtk_cfg_change_station
#define mtk_cfg_op_add_station		mtk_cfg_add_station
#define mtk_cfg_op_del_station		mtk_cfg_del_station
#endif

#if KERNEL_VERSION(6, 5, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_tdls_mgmt(struct wiphy *wiphy, struct net_device *dev,
			 const u8 *peer, int link_id, u8 action_code,
			 u8 dialog_token, u16 status_code,
			 u32 peer_capability, bool initiator,
			 const u8 *buf, size_t len);
#else
#define mtk_cfg_op_tdls_mgmt		mtk_cfg_tdls_mgmt
#endif

#if KERNEL_VERSION(6, 12, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_start_radar_detection(struct wiphy *wiphy,
				     struct net_device *dev,
				     struct cfg80211_chan_def *chandef,
				     u32 cac_time_ms, int link_id);
#else
#define mtk_cfg_op_start_radar_detection mtk_cfg_start_radar_detection
#endif

#if KERNEL_VERSION(6, 7, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_change_beacon(struct wiphy *wiphy, struct net_device *dev,
			     struct cfg80211_ap_update *info);
#else
#define mtk_cfg_op_change_beacon	mtk_cfg_change_beacon
#endif

#if KERNEL_VERSION(6, 0, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_stop_ap(struct wiphy *wiphy, struct net_device *dev,
		       unsigned int link_id);
int mtk_cfg_op_set_bitrate_mask(struct wiphy *wiphy, struct net_device *dev,
				unsigned int link_id, const u8 *peer,
				const struct cfg80211_bitrate_mask *mask);
#else
#define mtk_cfg_op_stop_ap		mtk_cfg_stop_ap
#define mtk_cfg_op_set_bitrate_mask	mtk_cfg_set_bitrate_mask
#endif

/* tx power and wiphy params: link_id (6.14), radio_idx (6.16) */
#if KERNEL_VERSION(6, 16, 0) <= CFG80211_VERSION_CODE
int mtk_cfg_op_set_wiphy_params(struct wiphy *wiphy, int radio_idx,
				u32 changed);
int mtk_cfg_op_set_txpower(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int radio_idx, enum nl80211_tx_power_setting type,
			   int mbm);
int mtk_cfg_op_get_txpower(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int radio_idx, unsigned int link_id, int *dbm);
#elif KERNEL_VERSION(6, 14, 0) <= CFG80211_VERSION_CODE
#define mtk_cfg_op_set_wiphy_params	mtk_cfg_set_wiphy_params
#define mtk_cfg_op_set_txpower		mtk_cfg_set_txpower
int mtk_cfg_op_get_txpower(struct wiphy *wiphy, struct wireless_dev *wdev,
			   unsigned int link_id, int *dbm);
#else
#define mtk_cfg_op_set_wiphy_params	mtk_cfg_set_wiphy_params
#define mtk_cfg_op_set_txpower		mtk_cfg_set_txpower
#define mtk_cfg_op_get_txpower		mtk_cfg_get_txpower
#endif

#endif /* _GL_CFG80211_COMPAT_H */

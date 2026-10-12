/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

// The part of the MediaTek gen4m NL80211_CMD_VENDOR interface used by this
// HAL (drivers/staging/mtk-connectivity/gen4m). The IDs are copied from
// os/linux/include/gl_vendor.h. The command table is mtk_wlan_vendor_ops in
// os/linux/gl_init.c. Every command used here is registered with
// WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV, so requests
// carry NL80211_ATTR_IFINDEX.
namespace gen4m {

constexpr uint32_t kGoogleOui = 0x001A11;

// enum WIFI_SUB_COMMAND, enum LSTATS_SUB_COMMAND, enum DEBUG_SUB_COMMAND.
constexpr uint32_t kSubcmdGetChannelList = 0x0001;
constexpr uint32_t kSubcmdGetFeatureSet = 0x0002;
constexpr uint32_t kSubcmdSetCountryCode = 0x0006;
constexpr uint32_t kSubcmdGetLinkStats = 0x1200;
constexpr uint32_t kSubcmdLoggerGetVersion = 0x1401;

// enum WIFI_ATTRIBUTE (nla_parse_wifi_attribute policy).
constexpr int kAttrBand = 1;          // NLA_U32, request
constexpr int kAttrNumChannels = 2;   // u32, reply
constexpr int kAttrChannelList = 3;   // u32[] in MHz, reply
constexpr int kAttrCountryCode = 8;   // NLA_STRING, request

// enum LOGGER_ATTRIBUTE (nla_get_version_policy). The handler only looks at
// the type of the first attribute, the payload is ignored.
constexpr int kLoggerAttrDriverVersion = 1;
constexpr int kLoggerAttrFirmwareVersion = 2;

// Band values understood by mtk_cfg80211_vendor_get_channel_list
// (os/linux/gl_vendor.c:343-357). They are not wifi_band: 3 means 6 GHz there,
// not WIFI_BAND_ABG, and any other value returns an empty list.
constexpr uint32_t kBand24Ghz = 1;
constexpr uint32_t kBand5GhzNoDfs = 2;
constexpr uint32_t kBand5GhzDfsOnly = 4;

}  // namespace gen4m

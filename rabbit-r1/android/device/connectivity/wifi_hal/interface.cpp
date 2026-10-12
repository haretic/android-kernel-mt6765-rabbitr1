/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "interface.h"

#include "gen4m_vendor.h"
#include "log.h"

#include <net/if.h>
#include <stddef.h>
#include <string.h>

#include <netlink/attr.h>
#include <netlink/msg.h>

#include <algorithm>

// Feature bits that gen4m may report (WIFI_HAL_FEATURE_SET in
// include/wlan_lib.h) but whose legacy HAL entry points this library leaves
// as NOT_SUPPORTED stubs. Advertising them would only send the framework into
// those stubs.
static const feature_set kUnimplementedFeatures =
    WIFI_FEATURE_GSCAN |
    WIFI_FEATURE_NAN |
    WIFI_FEATURE_D2D_RTT |
    WIFI_FEATURE_D2AP_RTT |
    WIFI_FEATURE_LOGGER |
    WIFI_FEATURE_HAL_EPNO |
    WIFI_FEATURE_RSSI_MONITOR |
    WIFI_FEATURE_MKEEP_ALIVE |
    WIFI_FEATURE_CONFIG_NDO |
    WIFI_FEATURE_TX_TRANSMIT_POWER |
    WIFI_FEATURE_CONTROL_ROAMING |
    WIFI_FEATURE_SET_TX_POWER_LIMIT |
    WIFI_FEATURE_USE_BODY_HEAD_SAR
#if !defined(__LP64__)
    | WIFI_FEATURE_LINK_LAYER_STATS
#endif
    ;

#if defined(__LP64__)
// gen4m fills its link layer stats reply by copying its own copies of the
// legacy HAL structures (STATS_LLS_* in os/linux/include/gl_vendor.h, same
// members in the same order). The layouts only match between an arm64
// kernel and 64-bit userspace, so pin the sizes the parser relies on.
static_assert(sizeof(wifi_iface_stat) == 376, "wifi_iface_stat layout");
static_assert(sizeof(wifi_peer_info) == 28, "wifi_peer_info layout");
static_assert(sizeof(wifi_rate_stat) == 32, "wifi_rate_stat layout");
static_assert(sizeof(wifi_radio_stat) == 56, "wifi_radio_stat layout");
static_assert(sizeof(wifi_channel_stat) == 24, "wifi_channel_stat layout");
// Radios are handed to the framework as one packed array, which keeps every
// wifi_radio_stat 8-byte aligned only if both strides are multiples of 8.
static_assert(sizeof(wifi_radio_stat) % 8 == 0 &&
              sizeof(wifi_channel_stat) % 8 == 0, "radio stride alignment");

// gen4m reports ENUM_BAND_NUM (2) radios, anything far above is corrupt.
static const uint32_t kMaxRadios = 8;

// Splits a gen4m LSTATS_SUBCMD_GET_INFO reply (mtk_cfg80211_vendor_llstats_get_info,
// os/linux/gl_vendor.c:1438-1443) into 8-byte aligned copies of
//   wifi_iface_stat, num_peers x (wifi_peer_info, num_rate x wifi_rate_stat)
// and
//   num_radios x (wifi_radio_stat, num_channels x wifi_channel_stat)
// with the u32 num_radios between the two. Netlink attribute data is only
// 4-byte aligned while these structures hold 64-bit members and pointers.
static bool parseLinkStats(const uint8_t* data,
                           size_t size,
                           std::vector<uint64_t>* ifaceBuffer,
                           std::vector<uint64_t>* radioBuffer,
                           int* numRadios) {
    size_t offset = sizeof(wifi_iface_stat);
    if (size < offset) {
        return false;
    }
    uint32_t numPeers;
    memcpy(&numPeers, data + offsetof(wifi_iface_stat, num_peers),
           sizeof(numPeers));
    for (uint32_t i = 0; i < numPeers; ++i) {
        if (size - offset < sizeof(wifi_peer_info)) {
            return false;
        }
        uint32_t numRates;
        memcpy(&numRates, data + offset + offsetof(wifi_peer_info, num_rate),
               sizeof(numRates));
        offset += sizeof(wifi_peer_info);
        if (numRates > (size - offset) / sizeof(wifi_rate_stat)) {
            return false;
        }
        offset += numRates * sizeof(wifi_rate_stat);
    }
    const size_t ifaceSize = offset;

    uint32_t radios;
    if (size - offset < sizeof(radios)) {
        return false;
    }
    memcpy(&radios, data + offset, sizeof(radios));
    offset += sizeof(radios);
    if (radios == 0 || radios > kMaxRadios) {
        return false;
    }
    const size_t radioStart = offset;
    for (uint32_t i = 0; i < radios; ++i) {
        if (size - offset < sizeof(wifi_radio_stat)) {
            return false;
        }
        uint32_t numChannels;
        memcpy(&numChannels,
               data + offset + offsetof(wifi_radio_stat, num_channels),
               sizeof(numChannels));
        offset += sizeof(wifi_radio_stat);
        if (numChannels > (size - offset) / sizeof(wifi_channel_stat)) {
            return false;
        }
        offset += numChannels * sizeof(wifi_channel_stat);
    }
    const size_t radioSize = offset - radioStart;

    ifaceBuffer->assign((ifaceSize + 7) / 8, 0);
    memcpy(ifaceBuffer->data(), data, ifaceSize);
    radioBuffer->assign((radioSize + 7) / 8, 0);
    memcpy(radioBuffer->data(), data + radioStart, radioSize);

    // gen4m copies tx_time_per_levels verbatim from firmware memory
    // (fill_radio). It is not a pointer in our address space, and the
    // framework dereferences it when num_tx_levels is non-zero.
    auto radio = reinterpret_cast<uint8_t*>(radioBuffer->data());
    for (uint32_t i = 0; i < radios; ++i) {
        auto stat = reinterpret_cast<wifi_radio_stat*>(radio);
        stat->num_tx_levels = 0;
        stat->tx_time_per_levels = nullptr;
        radio += sizeof(wifi_radio_stat) +
                 stat->num_channels * sizeof(wifi_channel_stat);
    }
    *numRadios = static_cast<int>(radios);
    return true;
}
#endif  // __LP64__

Interface::Interface(Nl80211& nl80211, const char* name)
    : mNl80211(nl80211)
    , mName(name)
    , mInterfaceIndex(0) {
}

Interface::Interface(Interface&& other) noexcept
    : mNl80211(other.mNl80211)
    , mName(std::move(other.mName))
    , mInterfaceIndex(other.mInterfaceIndex) {
}

bool Interface::init() {
    mInterfaceIndex = if_nametoindex(mName.c_str());
    if (mInterfaceIndex == 0) {
        ALOGE("Unable to get interface index for %s", mName.c_str());
        return false;
    }
    return true;
}

wifi_error Interface::vendorCommand(uint32_t subcmd,
                                    const Nl80211::AttributeWriter& writer,
                                    const Nl80211::ReplyHandler& handler) {
    return mNl80211.vendorCommand(mInterfaceIndex, gen4m::kGoogleOui, subcmd,
                                  writer, handler);
}

wifi_error Interface::getSupportedFeatureSet(feature_set* set) {
    if (set == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    // The reply is the raw u64 (nla_put_nohdr, gl_vendor.c:2089):
    // WIFI_HAL_FEATURE_SET, INFRA_5G when the NVRAM enables 5 GHz and
    // LINK_LAYER_STATS when the firmware stats buffer is mapped
    // (wlanGetSupportedFeatureSet, common/wlan_lib.c:12393).
    uint64_t driverSet = 0;
    bool haveReply = false;
    wifi_error status = vendorCommand(
        gen4m::kSubcmdGetFeatureSet, nullptr,
        [&driverSet, &haveReply](nlattr* data) {
            if (nla_len(data) >= static_cast<int>(sizeof(driverSet))) {
                memcpy(&driverSet, nla_data(data), sizeof(driverSet));
                haveReply = true;
            }
        });
    if (status != WIFI_SUCCESS || !haveReply) {
        // Keep the chip usable: the AIDL HAL fails IWifiChip::getFeatureSet
        // on an error here, while 0 (what goldfish returns) is accepted.
        ALOGE("Unable to get the driver feature set (%d)", status);
        *set = WIFI_FEATURE_INFRA;
        return WIFI_SUCCESS;
    }
    *set = (driverSet | WIFI_FEATURE_INFRA) & ~kUnimplementedFeatures;
    return WIFI_SUCCESS;
}

wifi_error Interface::getName(char* name, size_t size) {
    if (size < mName.size() + 1) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    strlcpy(name, mName.c_str(), size);
    return WIFI_SUCCESS;
}

// Wifi legacy HAL implicitly assumes getLinkStats is blocking and
// handler will be set to nullptr immediately after invocation.
wifi_error Interface::getLinkStats(wifi_request_id requestId,
                                   wifi_stats_result_handler handler) {
#if defined(__LP64__)
    if (handler.on_link_stats_results == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    std::vector<uint64_t> ifaceBuffer;
    std::vector<uint64_t> radioBuffer;
    int numRadios = 0;
    bool valid = false;
    // VENDOR_CMD_RAW_DATA: the driver fails the command if any vendor data
    // is attached (gl_vendor.c:1429).
    wifi_error status = vendorCommand(
        gen4m::kSubcmdGetLinkStats, nullptr,
        [&](nlattr* data) {
            valid = parseLinkStats(static_cast<const uint8_t*>(nla_data(data)),
                                   nla_len(data), &ifaceBuffer, &radioBuffer,
                                   &numRadios);
        });
    if (status != WIFI_SUCCESS) {
        return status;
    }
    if (!valid) {
        ALOGE("Invalid link layer stats reply");
        return WIFI_ERROR_UNKNOWN;
    }
    auto ifaceStats = reinterpret_cast<wifi_iface_stat*>(ifaceBuffer.data());
    ifaceStats->iface = reinterpret_cast<wifi_interface_handle>(this);
    handler.on_link_stats_results(
        requestId, ifaceStats, numRadios,
        reinterpret_cast<wifi_radio_stat*>(radioBuffer.data()));
    return WIFI_SUCCESS;
#else
    (void)requestId;
    (void)handler;
    return WIFI_ERROR_NOT_SUPPORTED;
#endif
}

wifi_error Interface::setLinkStats(wifi_link_layer_params /*params*/) {
    // gen4m has no command to start collection: the firmware keeps filling
    // the shared stats buffer that LSTATS_SUBCMD_GET_INFO reads.
    return WIFI_SUCCESS;
}

wifi_error Interface::getVersion(int attribute, char* buffer, size_t size) {
    if (buffer == nullptr || size == 0) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    // The reply is the bare string, not NUL terminated, padded to 4 bytes
    // (nla_put_nohdr, gl_vendor.c:2020).
    std::string version;
    wifi_error status = vendorCommand(
        gen4m::kSubcmdLoggerGetVersion,
        [attribute](nl_msg* msg) {
            return nla_put_u32(msg, attribute, 0) == 0;
        },
        [&version](nlattr* data) {
            const char* text = static_cast<const char*>(nla_data(data));
            version.assign(text, strnlen(text, nla_len(data)));
        });
    if (status != WIFI_SUCCESS) {
        return status;
    }
    if (version.empty()) {
        return WIFI_ERROR_UNKNOWN;
    }
    strlcpy(buffer, version.c_str(), size);
    return WIFI_SUCCESS;
}

wifi_error Interface::getFirmwareVersion(char* buffer, size_t size) {
    return getVersion(gen4m::kLoggerAttrFirmwareVersion, buffer, size);
}

wifi_error Interface::getDriverVersion(char* buffer, size_t size) {
    return getVersion(gen4m::kLoggerAttrDriverVersion, buffer, size);
}

wifi_error Interface::getDriverChannels(uint32_t driverBand,
                                        std::vector<wifi_channel>* channels) {
    bool haveReply = false;
    wifi_error status = vendorCommand(
        gen4m::kSubcmdGetChannelList,
        [driverBand](nl_msg* msg) {
            return nla_put_u32(msg, gen4m::kAttrBand, driverBand) == 0;
        },
        [channels, &haveReply](nlattr* data) {
            nlattr* attributes[gen4m::kAttrChannelList + 1];
            if (nla_parse(attributes, gen4m::kAttrChannelList,
                          static_cast<nlattr*>(nla_data(data)), nla_len(data),
                          nullptr) < 0 ||
                attributes[gen4m::kAttrNumChannels] == nullptr ||
                attributes[gen4m::kAttrChannelList] == nullptr) {
                return;
            }
            nlattr* list = attributes[gen4m::kAttrChannelList];
            const size_t count = std::min<size_t>(
                nla_get_u32(attributes[gen4m::kAttrNumChannels]),
                nla_len(list) / sizeof(uint32_t));
            const uint8_t* frequencies =
                static_cast<const uint8_t*>(nla_data(list));
            for (size_t i = 0; i < count; ++i) {
                uint32_t frequency;
                memcpy(&frequency, frequencies + i * sizeof(frequency),
                       sizeof(frequency));
                channels->push_back(static_cast<wifi_channel>(frequency));
            }
            haveReply = true;
        });
    if (status == WIFI_SUCCESS && !haveReply) {
        ALOGE("Invalid channel list reply for band %u", driverBand);
        return WIFI_ERROR_UNKNOWN;
    }
    return status;
}

wifi_error Interface::getValidChannels(int band,
                                       int maxChannels,
                                       wifi_channel* channels,
                                       int* numChannels) {
    if (channels == nullptr || numChannels == nullptr || maxChannels < 0) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    // The AIDL HAL checks *numChannels even when this fails.
    *numChannels = 0;
    if (band == WIFI_BAND_UNSPECIFIED) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    // Query gen4m once per wifi_band bit. Bits above WIFI_BAND_A_DFS
    // (6 GHz, 60 GHz) have no channels on this chip.
    static const struct {
        int bandBit;
        uint32_t driverBand;
    } kBands[] = {
        { WIFI_BAND_BG, gen4m::kBand24Ghz },
        { WIFI_BAND_A, gen4m::kBand5GhzNoDfs },
        { WIFI_BAND_A_DFS, gen4m::kBand5GhzDfsOnly },
    };
    std::vector<wifi_channel> result;
    for (const auto& entry : kBands) {
        if ((band & entry.bandBit) == 0) {
            continue;
        }
        wifi_error status = getDriverChannels(entry.driverBand, &result);
        if (status != WIFI_SUCCESS) {
            return status;
        }
    }

    // goldfish copied *numChannels bytes here instead of elements.
    *numChannels = static_cast<int>(
        std::min<size_t>(result.size(), static_cast<size_t>(maxChannels)));
    std::copy_n(result.begin(), *numChannels, channels);
    return WIFI_SUCCESS;
}

wifi_error Interface::setCountryCode(const char* countryCode) {
    if (countryCode == nullptr || strnlen(countryCode, 2) < 2) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    // The driver takes the first two bytes of the attribute
    // (gl_vendor.c:441).
    const char code[3] = { countryCode[0], countryCode[1], '\0' };
    return vendorCommand(
        gen4m::kSubcmdSetCountryCode,
        [&code](nl_msg* msg) {
            return nla_put(msg, gen4m::kAttrCountryCode, sizeof(code),
                           code) == 0;
        },
        nullptr);
}

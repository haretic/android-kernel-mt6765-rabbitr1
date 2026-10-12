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

#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <hardware_legacy/wifi_hal.h>

#include "nl80211.h"

class Interface {
public:
    Interface(Nl80211& nl80211, const char* name);
    Interface(Interface&& other) noexcept;

    bool init();

    wifi_error getSupportedFeatureSet(feature_set* set);
    wifi_error getName(char* name, size_t size);
    wifi_error getLinkStats(wifi_request_id requestId,
                            wifi_stats_result_handler handler);
    wifi_error setLinkStats(wifi_link_layer_params params);
    wifi_error getFirmwareVersion(char* buffer, size_t size);
    wifi_error getDriverVersion(char* buffer, size_t size);
    wifi_error getValidChannels(int band,
                                int maxChannels,
                                wifi_channel* channels,
                                int* numChannels);
    wifi_error setCountryCode(const char* countryCode);

private:
    Interface(const Interface&) = delete;
    Interface& operator=(const Interface&) = delete;

    wifi_error vendorCommand(uint32_t subcmd,
                             const Nl80211::AttributeWriter& writer,
                             const Nl80211::ReplyHandler& handler);
    wifi_error getVersion(int attribute, char* buffer, size_t size);
    wifi_error getDriverChannels(uint32_t driverBand,
                                 std::vector<wifi_channel>* channels);

    Nl80211& mNl80211;
    std::string mName;
    uint32_t mInterfaceIndex;
};

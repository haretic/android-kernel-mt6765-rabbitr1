/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include <functional>
#include <mutex>

#include <hardware_legacy/wifi_hal.h>

struct nl_msg;
struct nl_sock;
struct nlattr;

// Synchronous NL80211_CMD_VENDOR client on its own generic netlink socket.
// Requests are sent and their replies collected on the calling thread, so
// the event loop is not involved and a missing reply cannot block it.
class Nl80211 {
public:
    // Appends the attributes nested in NL80211_ATTR_VENDOR_DATA. Returns
    // false if the message ran out of space.
    using AttributeWriter = std::function<bool (nl_msg* msg)>;
    // Called with the NL80211_ATTR_VENDOR_DATA attribute of the reply. The
    // payload is only valid during the call.
    using ReplyHandler = std::function<void (nlattr* vendorData)>;

    Nl80211();
    ~Nl80211();

    bool init();

    // A null |writer| sends the command without NL80211_ATTR_VENDOR_DATA,
    // as VENDOR_CMD_RAW_DATA commands require: cfg80211 rejects a nested
    // attribute for them. A non-null |writer| sends a nested
    // NL80211_ATTR_VENDOR_DATA, which commands with a policy require.
    wifi_error vendorCommand(uint32_t ifIndex,
                             uint32_t vendorId,
                             uint32_t subcmd,
                             const AttributeWriter& writer,
                             const ReplyHandler& handler);

private:
    Nl80211(const Nl80211&) = delete;
    Nl80211& operator=(const Nl80211&) = delete;

    bool openSocketLocked();
    void closeSocketLocked();

    std::mutex mMutex;
    nl_sock* mSocket;
    int mFamilyId;
};

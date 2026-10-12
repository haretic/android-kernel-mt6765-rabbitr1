/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nl80211.h"

#include "log.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <netlink/attr.h>
#include <netlink/errno.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/handlers.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>

#include <memory>

namespace {

// Upper bound for one vendor command. gen4m answers from kalIoctl, which has
// its own timeouts, so this only protects the HAL from a wedged driver.
constexpr time_t kReplyTimeoutSeconds = 5;

struct RequestState {
    // > 0 while waiting, 0 on success, -errno from the kernel on failure.
    int result = 1;
    const Nl80211::ReplyHandler* handler = nullptr;
};

int onValid(nl_msg* msg, void* arg) {
    auto state = static_cast<RequestState*>(arg);
    auto genlHeader = static_cast<genlmsghdr*>(nlmsg_data(nlmsg_hdr(msg)));
    nlattr* attributes[NL80211_ATTR_MAX + 1];

    if (nla_parse(attributes, NL80211_ATTR_MAX,
                  genlmsg_attrdata(genlHeader, 0),
                  genlmsg_attrlen(genlHeader, 0), nullptr) < 0) {
        ALOGE("Unable to parse nl80211 vendor reply");
        return NL_SKIP;
    }
    nlattr* vendorData = attributes[NL80211_ATTR_VENDOR_DATA];
    if (vendorData != nullptr && *state->handler) {
        (*state->handler)(vendorData);
    }
    return NL_SKIP;
}

int onError(sockaddr_nl* /*address*/, nlmsgerr* error, void* arg) {
    static_cast<RequestState*>(arg)->result = error->error;
    return NL_STOP;
}

int onAck(nl_msg* /*msg*/, void* arg) {
    static_cast<RequestState*>(arg)->result = 0;
    return NL_STOP;
}

int onFinish(nl_msg* /*msg*/, void* arg) {
    static_cast<RequestState*>(arg)->result = 0;
    return NL_SKIP;
}

wifi_error toWifiError(int error) {
    switch (error) {
        case 0:
            return WIFI_SUCCESS;
        case -EOPNOTSUPP:
            return WIFI_ERROR_NOT_SUPPORTED;
        case -EINVAL:
            return WIFI_ERROR_INVALID_ARGS;
        case -ENOMEM:
            return WIFI_ERROR_OUT_OF_MEMORY;
        case -EBUSY:
            return WIFI_ERROR_BUSY;
        case -EAGAIN:
        case -ETIMEDOUT:
            return WIFI_ERROR_TIMED_OUT;
        case -ENODEV:
        case -ENETDOWN:
            return WIFI_ERROR_NOT_AVAILABLE;
        default:
            return WIFI_ERROR_UNKNOWN;
    }
}

}  // namespace

Nl80211::Nl80211() : mSocket(nullptr), mFamilyId(-1) {
}

Nl80211::~Nl80211() {
    std::lock_guard<std::mutex> lock(mMutex);
    closeSocketLocked();
}

bool Nl80211::init() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mSocket != nullptr || openSocketLocked();
}

bool Nl80211::openSocketLocked() {
    mSocket = nl_socket_alloc();
    if (mSocket == nullptr) {
        ALOGE("Unable to allocate netlink socket");
        return false;
    }
    int status = genl_connect(mSocket);
    if (status < 0) {
        ALOGE("Unable to connect generic netlink socket: %s",
              nl_geterror(status));
        closeSocketLocked();
        return false;
    }
    // Link layer stats replies can be larger than libnl's default receive
    // buffer, let libnl peek at the size first.
    nl_socket_enable_msg_peek(mSocket);

    struct timeval timeout = {};
    timeout.tv_sec = kReplyTimeoutSeconds;
    if (setsockopt(nl_socket_get_fd(mSocket), SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) != 0) {
        ALOGW("Unable to set netlink receive timeout: %s", strerror(errno));
    }

    mFamilyId = genl_ctrl_resolve(mSocket, "nl80211");
    if (mFamilyId < 0) {
        ALOGE("Unable to resolve nl80211: %s", nl_geterror(mFamilyId));
        closeSocketLocked();
        return false;
    }
    return true;
}

void Nl80211::closeSocketLocked() {
    if (mSocket != nullptr) {
        nl_socket_free(mSocket);
        mSocket = nullptr;
    }
    mFamilyId = -1;
}

wifi_error Nl80211::vendorCommand(uint32_t ifIndex,
                                  uint32_t vendorId,
                                  uint32_t subcmd,
                                  const AttributeWriter& writer,
                                  const ReplyHandler& handler) {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mSocket == nullptr && !openSocketLocked()) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    std::unique_ptr<nl_msg, decltype(&nlmsg_free)> msg(nlmsg_alloc(),
                                                        nlmsg_free);
    if (!msg) {
        return WIFI_ERROR_OUT_OF_MEMORY;
    }
    if (genlmsg_put(msg.get(), NL_AUTO_PORT, NL_AUTO_SEQ, mFamilyId, 0, 0,
                    NL80211_CMD_VENDOR, 0) == nullptr ||
        nla_put_u32(msg.get(), NL80211_ATTR_IFINDEX, ifIndex) < 0 ||
        nla_put_u32(msg.get(), NL80211_ATTR_VENDOR_ID, vendorId) < 0 ||
        nla_put_u32(msg.get(), NL80211_ATTR_VENDOR_SUBCMD, subcmd) < 0) {
        return WIFI_ERROR_OUT_OF_MEMORY;
    }
    if (writer) {
        // nla_nest_start() sets NLA_F_NESTED, which cfg80211 requires for
        // vendor commands that have a policy.
        nlattr* vendorData = nla_nest_start(msg.get(),
                                            NL80211_ATTR_VENDOR_DATA);
        if (vendorData == nullptr || !writer(msg.get())) {
            return WIFI_ERROR_OUT_OF_MEMORY;
        }
        nla_nest_end(msg.get(), vendorData);
    }

    std::unique_ptr<nl_cb, decltype(&nl_cb_put)> cb(nl_cb_alloc(NL_CB_DEFAULT),
                                                    nl_cb_put);
    if (!cb) {
        return WIFI_ERROR_OUT_OF_MEMORY;
    }
    RequestState state;
    state.handler = &handler;
    nl_cb_err(cb.get(), NL_CB_CUSTOM, onError, &state);
    nl_cb_set(cb.get(), NL_CB_ACK, NL_CB_CUSTOM, onAck, &state);
    nl_cb_set(cb.get(), NL_CB_FINISH, NL_CB_CUSTOM, onFinish, &state);
    nl_cb_set(cb.get(), NL_CB_VALID, NL_CB_CUSTOM, onValid, &state);

    int status = nl_send_auto(mSocket, msg.get());
    if (status < 0) {
        ALOGE("Unable to send vendor command 0x%x: %s", subcmd,
              nl_geterror(status));
        closeSocketLocked();
        return WIFI_ERROR_UNKNOWN;
    }
    while (state.result > 0) {
        status = nl_recvmsgs(mSocket, cb.get());
        // nl_recvmsgs() also fails when onError() stops it, that case is
        // handled through state.result below.
        if (status < 0 && state.result > 0) {
            ALOGE("No reply to vendor command 0x%x: %s", subcmd,
                  nl_geterror(status));
            // Drop the socket, a late reply must not be taken for the reply
            // to the next request.
            closeSocketLocked();
            return status == -NLE_AGAIN ? WIFI_ERROR_TIMED_OUT
                                        : WIFI_ERROR_UNKNOWN;
        }
    }
    if (state.result < 0) {
        ALOGW("Vendor command 0x%x failed: %s", subcmd,
              strerror(-state.result));
    }
    return toWifiError(state.result);
}

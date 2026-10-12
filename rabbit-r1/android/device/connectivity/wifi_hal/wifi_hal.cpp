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

#include <condition_variable>
#include <memory>
#include <mutex>
#include "halstate.h"
#include "info.h"
#include "interface.h"

// This library is built with -fvisibility=hidden: the AIDL service loads it
// with dlopen() and only looks up init_wifi_vendor_hal_func_table, and the
// hidden wifi_* definitions below cannot be interposed by a libwifi-hal.so
// that statically links another vendor HAL into the service.

HalState* asHalState(wifi_handle h) {
    return reinterpret_cast<HalState*>(h);
}

Info* asInfo(wifi_handle h) {
    return asHalState(h)->info();
}

Interface* asInterface(wifi_interface_handle h) {
    return reinterpret_cast<Interface*>(h);
}

wifi_error wifi_initialize(wifi_handle* handle) {
    if (handle == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    // Make the HAL state static inside the function for lazy construction. When
    // stopping we want to keep track of the current HAL state because if the
    // HAL starts again we need to know if we're in a state where we can start
    // or not. If we're stopping with the intention of never starting back up
    // again we could destroy the HAL state. Unfortunately there is no
    // distinction between these two events so the safe choice is to leak this
    // memory and always keep track of the HAL state. This is allocated on the
    // heap instead of the stack to prevent any destructors being called when
    // the dynamic library is being unloaded since the program state could be
    // unreliable at this point.
    static HalState* sHalState = new HalState();

    if (!sHalState->init()) {
        return WIFI_ERROR_UNKNOWN;
    }
    *handle = reinterpret_cast<wifi_handle>(sHalState);

    return WIFI_SUCCESS;
}

void wifi_cleanup(wifi_handle handle, wifi_cleaned_up_handler handler) {
    if (handle == nullptr) {
        return;
    }

    std::condition_variable condition;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    bool stopped = false;
    // This lambda will be called when the stop completes. That will notify the
    // condition variable and this function will wake up and exit. This ensures
    // that this function is synchronous. The boolean is to ensure that when
    // waiting we're protected against spurious wakeups, we only exit once the
    // callback has signaled that it's been called.
    auto callback = [&mutex, &stopped, &condition] {
        std::unique_lock<std::mutex> lock(mutex);
        stopped = true;
        condition.notify_all();
    };
    if (asHalState(handle)->stop(callback)) {
        // The handler succeeded and will call our callback, wait for it. If the
        // stop call did not succeed we can't wait for this condition since our
        // callback will never call notify on it.
        while (!stopped) {
            condition.wait(lock);
        }
    }
    // The HAL seems to expect this callback to happen on the same thread, or at
    // least that's what happens in other WiFi HALs. This is why this method has
    // to be synchronous.
    handler(handle);
}

void wifi_event_loop(wifi_handle handle) {
    if (handle == nullptr) {
        return;
    }

    asHalState(handle)->eventLoop();
}

wifi_error wifi_get_supported_feature_set(wifi_interface_handle handle,
                                          feature_set* set) {
    if (handle == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->getSupportedFeatureSet(set);
}

wifi_error wifi_get_ifaces(wifi_handle handle,
                           int* num,
                           wifi_interface_handle** interfaces) {
    if (handle == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInfo(handle)->getInterfaces(num, interfaces);
}

wifi_error wifi_get_iface_name(wifi_interface_handle handle,
                               char* name,
                               size_t size) {
    if (handle == nullptr || (name == nullptr && size > 0)) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->getName(name, size);
}

wifi_error wifi_get_link_stats(wifi_request_id id,
                               wifi_interface_handle handle,
                               wifi_stats_result_handler handler) {
    if (handle == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->getLinkStats(id, handler);
}

wifi_error wifi_set_link_stats(wifi_interface_handle handle,
                               wifi_link_layer_params params) {
    if (handle == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->setLinkStats(params);
}

wifi_error wifi_get_firmware_version(wifi_interface_handle handle,
                                     char* buffer,
                                     int buffer_size) {
    if (handle == nullptr || buffer_size <= 0) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->getFirmwareVersion(buffer, buffer_size);
}

wifi_error wifi_get_driver_version(wifi_interface_handle handle,
                                   char* buffer,
                                   int buffer_size) {
    if (handle == nullptr || buffer_size <= 0) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->getDriverVersion(buffer, buffer_size);
}

wifi_error wifi_get_valid_channels(wifi_interface_handle handle,
                                   int band,
                                   int max_channels,
                                   wifi_channel *channels,
                                   int *num_channels)
{
    if (handle == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->getValidChannels(band,
                                                 max_channels,
                                                 channels,
                                                 num_channels);
}

wifi_error wifi_set_country_code(wifi_interface_handle handle,
                                 const char *country_code) {
    if (handle == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    return asInterface(handle)->setCountryCode(country_code);
}

__attribute__((visibility("default")))
wifi_error init_wifi_vendor_hal_func_table(wifi_hal_fn* fn)
{
    if (fn == NULL) {
        return WIFI_ERROR_UNKNOWN;
    }
    // The AIDL service fills every slot with a stub returning
    // WIFI_ERROR_NOT_SUPPORTED (initHalFuncTableWithStubs) before calling
    // this, so only the implemented entry points are set. In particular
    // wifi_wait_for_driver_ready and wifi_virtual_interface_create stay
    // stubs, which the service accepts (see README.md).
    fn->wifi_initialize = wifi_initialize;
    fn->wifi_cleanup = wifi_cleanup;
    fn->wifi_event_loop = wifi_event_loop;
    fn->wifi_get_supported_feature_set = wifi_get_supported_feature_set;

    fn->wifi_get_ifaces = wifi_get_ifaces;
    fn->wifi_get_iface_name = wifi_get_iface_name;
    fn->wifi_get_link_stats = wifi_get_link_stats;
    fn->wifi_set_link_stats = wifi_set_link_stats;

    fn->wifi_get_firmware_version = wifi_get_firmware_version;
    fn->wifi_get_driver_version = wifi_get_driver_version;

    fn->wifi_get_valid_channels = wifi_get_valid_channels;
    fn->wifi_set_country_code = wifi_set_country_code;

    return WIFI_SUCCESS;
}

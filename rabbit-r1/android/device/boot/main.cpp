// SPDX-License-Identifier: Apache-2.0
#include "BootControl.h"
#include <cstdlib>
#include <cstring>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <bootloader_message/bootloader_message.h>

using aidl::android::hardware::boot::BootControl;
static_assert(BOOTLOADER_MESSAGE_OFFSET_IN_MISC == 0 && VENDOR_SPACE_OFFSET_IN_MISC == 2048,
              "r1 boot-control offsets must match the stock para layout");

// init writes "bootonce-bootloader" for reboot,bootloader, but LK enters fastboot
// from the RTC flag and never clears the BCB. Left there, it also stops init from
// writing boot-recovery, which it only puts into an empty command.
static void ClearStaleBootloaderCommand() {
    bootloader_message boot = {};
    std::string error;
    if (!read_bootloader_message(&boot, &error)) {
        LOG(WARNING) << "Cannot read the BCB: " << error;
        return;
    }
    if (strncmp(boot.command, "bootonce-bootloader", sizeof(boot.command)) != 0) return;
    memset(boot.command, 0, sizeof(boot.command));
    if (!write_bootloader_message(boot, &error))
        LOG(WARNING) << "Cannot clear bootonce-bootloader: " << error;
    else
        LOG(INFO) << "Cleared a stale bootonce-bootloader from the BCB";
}

int main(int, char* argv[]) {
    android::base::InitLogging(argv, android::base::KernelLogger);
    const std::string suffix = android::base::GetProperty("ro.boot.slot_suffix", "");
    CHECK(suffix == "_a" || suffix == "_b") << "Missing or invalid r1 slot suffix";
    std::string error;
    const std::string misc = get_bootloader_message_blk_device(&error);
    CHECK(!misc.empty()) << error;
    ClearStaleBootloaderCommand();
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto service = ndk::SharedRefBase::make<BootControl>(suffix == "_a" ? 0 : 1, misc);
    const std::string instance = std::string(BootControl::descriptor) + "/default";
    CHECK_EQ(AServiceManager_addService(service->asBinder().get(), instance.c_str()), STATUS_OK);
    LOG(INFO) << "haretic r1 boot-control service running";
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}

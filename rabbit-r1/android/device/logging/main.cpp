// SPDX-License-Identifier: Apache-2.0
#include "Expdb.h"
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <cstdlib>

int main(int argc, char* argv[]) {
    android::base::InitLogging(argv, android::base::KernelLogger);
    if (argc != 1 || android::base::GetProperty("ro.vendor.r1.expdb.enabled", "") != "1") {
        LOG(ERROR) << "haretic expdb: fixed build-profile opt-in is required";
        return EXIT_FAILURE;
    }
    std::string error;
    haretic::logging::Kedump kedump;
    if (!haretic::logging::ConsumeKedump(&kedump, &error)) {
        LOG(ERROR) << "haretic expdb: LK KEdump not consumed: " << error;
        return EXIT_FAILURE;
    }
    if (kedump == haretic::logging::Kedump::Consumed) {
        LOG(INFO) << "haretic expdb: consumed LK KEdump; ramoops already holds its console";
        return EXIT_SUCCESS;
    }
    if (kedump == haretic::logging::Kedump::Absent) {
        LOG(INFO) << "haretic expdb: no LK KEdump; ramoops is the pstore backend";
        return EXIT_SUCCESS;
    }
    if (!haretic::logging::StartExpdb(&error)) {
        LOG(ERROR) << "haretic expdb unavailable: " << error;
        return EXIT_FAILURE;
    }
    LOG(INFO) << "haretic expdb ready: verified 18 MiB pstore_blk map; 2 MiB tail excluded";
    return EXIT_SUCCESS;
}

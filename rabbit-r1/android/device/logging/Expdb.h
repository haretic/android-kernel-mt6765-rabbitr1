// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>
#include <sys/types.h>

namespace haretic::logging {
// This entry point accepts no paths, geometry, parameters or module names.
// A false result never authorizes use of the raw expdb partition for logging.
bool StartExpdb(std::string* error);

// After an abnormal reset LK saves the RAM console and pstore to expdb behind a
// KEdump header and restores them over ramoops on every later boot, until the
// header goes; stock AEE cleared it once booted. With ramoops as the pstore
// backend this clears the header (its first 4 KiB) and nothing else. Any other
// backend leaves expdb untouched and reports NotRamoops.
enum class Kedump { NotRamoops, Absent, Consumed };
bool ConsumeKedump(Kedump* result, std::string* error);

// Checks the actual DM_TABLE_STATUS response, including bounded string parsing.
bool ValidateExpdbTable(const void* buffer, size_t capacity, dev_t backing,
                        dev_t mapper, const std::string& uuid, std::string* error);
}  // namespace haretic::logging

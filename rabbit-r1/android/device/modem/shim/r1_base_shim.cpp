// SPDX-License-Identifier: Apache-2.0
//
// libnvram.so and libsysenv.so (Android 12) import the non-template
// android::base::Trim(const std::string&) and Basename(const std::string&).
// Current libbase only has Trim<T>(T&&) instantiations and
// Basename(std::string_view), which mangle differently. Forward to them.

#include <string>
#include <string_view>

#include <android-base/file.h>
#include <android-base/strings.h>

namespace android {
namespace base {

// Declared here only: these are the old non-template overloads.
std::string Trim(const std::string& s);
std::string Basename(const std::string& path);

std::string Trim(const std::string& s) {
    // string_view does not convert to const std::string&, so this calls
    // the Trim<std::string_view> instantiation rather than recursing.
    return Trim(std::string_view(s));
}

std::string Basename(const std::string& path) {
    return Basename(std::string_view(path));
}

}  // namespace base
}  // namespace android

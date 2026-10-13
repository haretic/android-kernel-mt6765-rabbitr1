// SPDX-License-Identifier: Apache-2.0
//
// Android 12 symbols that the stock MediaTek Codec2 libraries import and that
// the current platform libraries no longer export. Each forwards to the API
// that replaced it:
//
//   android::base::WriteStringToFd(const std::string&, borrowed_fd)
//       libcodec2_hidl@1.x, libavservices_minijail_vendor. Now takes a
//       std::string_view.
//   android::GraphicBufferMapper::lock(handle, uint32_t usage, bounds, vaddr,
//                                      outBytesPerPixel, outBytesPerStride)
//   android::GraphicBufferMapper::unlock(handle)
//       libcodec2_vndk. Replaced by lock() returning a LockResult and by
//       unlock() with an optional release fence, which it waits for when
//       none is asked for, as the old unlock() did.
//   android::GraphicBufferSource::getHGraphicBufferProducer() const
//       libcodec2_hidl@1.x (HAL-side input surfaces). The bufferqueue 2.0
//       producer getter was removed; only the 1.0 one is left.
//
// One Android 12 class has its own implementation here, because its layout
// changed:
//
//   android::Fence(int), wait(int), getSignalTime() const
//       libcodec2_vndk, through C2BufferQueueBlockPool's dequeue fences. It
//       allocates the object itself, 8 bytes: the reference count and the fd.
//       The Android 17 class has virtual functions and is 12 bytes, so its
//       constructor wrote past the allocation and its getters read the fd
//       from beyond it. libcodec2_vndk imports these as r1compt::Fence
//       instead (blobs.txt), which keeps the Android 12 layout.
//
// The member functions are defined as free functions under the member's
// mangled name. That has the member's calling convention: the object is the
// first argument, and a returned sp<> goes through the same hidden result
// pointer for both.

#define LOG_TAG "r1_codec2_shim"

#include <errno.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <android-base/file.h>
#include <android/sync.h>
#include <log/log.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/bufferqueue/2.0/B2HGraphicBufferProducer.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>
#include <utils/Timers.h>

namespace android {
namespace base {

// Declared here only: this is the old overload.
bool WriteStringToFd(const std::string& content, borrowed_fd fd);

bool WriteStringToFd(const std::string& content, borrowed_fd fd) {
    // std::string_view does not convert to const std::string&, so this
    // calls the current overload rather than recursing.
    return WriteStringToFd(std::string_view(content), fd);
}

}  // namespace base

// libstagefright_bufferqueue_helper's class, only passed on by address. Its
// header depends on libgui feature flags; the shim needs none of it.
class GraphicBufferSource;

namespace {

using HGraphicBufferProducer = hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer;
using hardware::graphics::bufferqueue::V2_0::utils::B2HGraphicBufferProducer;

}  // namespace

status_t r1GraphicBufferMapperLock(GraphicBufferMapper* mapper, buffer_handle_t handle,
                                   uint32_t usage, const Rect& bounds, void** vaddr,
                                   int32_t* outBytesPerPixel, int32_t* outBytesPerStride)
        __asm__("_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_");

status_t r1GraphicBufferMapperLock(GraphicBufferMapper* mapper, buffer_handle_t handle,
                                   uint32_t usage, const Rect& bounds, void** vaddr,
                                   int32_t* outBytesPerPixel, int32_t* outBytesPerStride) {
    auto result = mapper->lock(handle, static_cast<int64_t>(usage), bounds);
    if (!result.has_value()) return result.asStatus();
    const GraphicBufferMapper::LockResult& locked = result.value();
    *vaddr = locked.address;
    if (outBytesPerPixel) *outBytesPerPixel = locked.bytesPerPixel;
    if (outBytesPerStride) *outBytesPerStride = locked.bytesPerStride;
    return OK;
}

status_t r1GraphicBufferMapperUnlock(GraphicBufferMapper* mapper, buffer_handle_t handle)
        __asm__("_ZN7android19GraphicBufferMapper6unlockEPK13native_handle");

status_t r1GraphicBufferMapperUnlock(GraphicBufferMapper* mapper, buffer_handle_t handle) {
    return mapper->unlock(handle, nullptr);
}

// GraphicBufferSource::getIGraphicBufferProducer() const, still exported by
// libstagefright_bufferqueue_helper.
sp<IGraphicBufferProducer> r1GraphicBufferSourceGetIProducer(const GraphicBufferSource* source)
        __asm__("_ZNK7android19GraphicBufferSource25getIGraphicBufferProducerEv");

sp<HGraphicBufferProducer> r1GraphicBufferSourceGetHProducer(const GraphicBufferSource* source)
        __asm__("_ZNK7android19GraphicBufferSource25getHGraphicBufferProducerEv");

sp<HGraphicBufferProducer> r1GraphicBufferSourceGetHProducer(const GraphicBufferSource* source) {
    // What the Android 12 implementation returned.
    return sp<B2HGraphicBufferProducer>::make(r1GraphicBufferSourceGetIProducer(source));
}

}  // namespace android

namespace r1compt {

// android::Fence of Android 12: LightRefBase's count, then the fd. Objects
// are created, reference-counted and destroyed by libcodec2_vndk's inline
// code; only these three members are called.
class Fence {
  public:
    explicit Fence(int fenceFd);
    android::status_t wait(int timeout);
    nsecs_t getSignalTime() const;

  private:
    mutable std::atomic<int32_t> mCount;
    int mFenceFd;
};

static_assert(sizeof(Fence) == 8 && sizeof(std::atomic<int32_t>) == 4, "Android 12 layout");

Fence::Fence(int fenceFd) : mCount(0), mFenceFd(fenceFd) {}

android::status_t Fence::wait(int timeout) {
    if (mFenceFd == -1) return android::NO_ERROR;
    int err = sync_wait(mFenceFd, timeout);
    return err < 0 ? -errno : android::NO_ERROR;
}

nsecs_t Fence::getSignalTime() const {
    constexpr nsecs_t kInvalid = -1, kPending = INT64_MAX;
    if (mFenceFd == -1) return kInvalid;
    struct sync_file_info* info = sync_file_info(mFenceFd);
    if (!info) {
        ALOGE("sync_file_info returned NULL for fd %d", mFenceFd);
        return kInvalid;
    }
    if (info->status != 1) {
        const int status = info->status;
        sync_file_info_free(info);
        return status < 0 ? kInvalid : kPending;
    }
    uint64_t timestamp = 0;
    struct sync_fence_info* fences = sync_get_fence_info(info);
    for (size_t i = 0; i < info->num_fences; i++)
        if (fences[i].timestamp_ns > timestamp) timestamp = fences[i].timestamp_ns;
    sync_file_info_free(info);
    return static_cast<nsecs_t>(timestamp);
}

}  // namespace r1compt

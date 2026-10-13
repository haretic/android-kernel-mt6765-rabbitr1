// SPDX-License-Identifier: Apache-2.0
// libdpframework for the stock MediaTek Codec2 libraries, converting with the
// CPU. The stock library drives the MDP through /dev/mtk_cmdq and
// /dev/mdp_sync, which the mainline kernel does not have; the encoder uses it
// to turn RGBA input (from an input surface) into the YUV its hardware reads.
//
// A job is converted synchronously in invalidate(), so the fence createJob
// hands out is already signalled: a sync_file exported from an idle dma-buf.
#define LOG_TAG "r1-dpframework"

#include "DpAsyncBlitStream.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <android/sync.h>
#include <libyuv.h>
#include <log/log.h>

namespace {

enum class Layout { RGB, I420, YV12, NV12, NV21, YUYV, UYVY };

struct Format {
    DP_COLOR_ENUM color;
    Layout layout;
    int bpp;  // bytes per pixel of plane 0
};

constexpr Format kFormats[] = {
        {DP_COLOR_RGB565, Layout::RGB, 2},   {DP_COLOR_RGB888, Layout::RGB, 3},
        {DP_COLOR_BGR888, Layout::RGB, 3},   {DP_COLOR_RGBA8888, Layout::RGB, 4},
        {DP_COLOR_BGRA8888, Layout::RGB, 4}, {DP_COLOR_ARGB8888, Layout::RGB, 4},
        {DP_COLOR_ABGR8888, Layout::RGB, 4}, {DP_COLOR_UYVY, Layout::UYVY, 2},
        {DP_COLOR_YUYV, Layout::YUYV, 2},    {DP_COLOR_I420, Layout::I420, 1},
        {DP_COLOR_YV12, Layout::YV12, 1},    {DP_COLOR_NV12, Layout::NV12, 1},
        {DP_COLOR_NV21, Layout::NV21, 1},
};

const Format* findFormat(DP_COLOR_ENUM color) {
    for (const Format& f : kFormats)
        if (f.color == color) return &f;
    return nullptr;
}

bool isPlanar(const Format* f) {
    return f->layout == Layout::I420 || f->layout == Layout::YV12;
}

bool isSemiPlanar(const Format* f) {
    return f->layout == Layout::NV12 || f->layout == Layout::NV21;
}

int planeCount(const Format* f) {
    return isPlanar(f) ? 3 : isSemiPlanar(f) ? 2 : 1;
}

// Plane 1 is always U (or interleaved chroma), plane 2 V.
struct Planes {
    uint8_t* p[3] = {};
    int stride[3] = {};
};

struct Port {
    int fd = -1;
    uint32_t sizes[3] = {};
    uint32_t planeNumber = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t yPitch = 0;
    int32_t uvPitch = 0;
    const Format* format = nullptr;
    DP_PROFILE_ENUM profile = DP_PROFILE_BT601;
    DpRect rect;  // source crop or destination ROI
    bool hasRect = false;
    int32_t rotation = 0;

    int32_t chromaPitch() const {
        if (uvPitch > 0) return uvPitch;
        return isPlanar(format) ? yPitch / 2 : yPitch;
    }
};

// A CPU mapping of a dma-buf, bracketed by DMA_BUF_IOCTL_SYNC.
class Mapping {
  public:
    Mapping(int fd, size_t length, bool write) : mFd(fd), mLength(length) {
        mFlags = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
        void* addr = mmap(nullptr, length, write ? PROT_READ | PROT_WRITE : PROT_READ,
                          MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            ALOGE("mmap fd %d, %zu bytes: %s", fd, length, strerror(errno));
            return;
        }
        mAddr = static_cast<uint8_t*>(addr);
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | mFlags};
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync))
            ALOGW("DMA_BUF_IOCTL_SYNC start fd %d: %s", fd, strerror(errno));
    }
    ~Mapping() {
        if (!mAddr) return;
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_END | mFlags};
        if (ioctl(mFd, DMA_BUF_IOCTL_SYNC, &sync))
            ALOGW("DMA_BUF_IOCTL_SYNC end fd %d: %s", mFd, strerror(errno));
        munmap(mAddr, mLength);
    }
    uint8_t* get() const { return mAddr; }

  private:
    int mFd;
    size_t mLength;
    uint64_t mFlags;
    uint8_t* mAddr = nullptr;
};

// Maps a port's buffer and finds its planes. The planes follow each other in
// the one dma-buf: at the offsets the size list gives, or, with fewer sizes
// than planes, packed at pitch times rows.
DP_STATUS_ENUM mapPort(const Port& port, bool write, std::unique_ptr<Mapping>* mapping,
                       Planes* planes) {
    if (port.fd < 0 || !port.format) return DP_STATUS_INVALID_STATE;
    const int n = planeCount(port.format);
    const int chromaRows = (port.height + 1) / 2;
    size_t offset[3] = {0, 0, 0};
    size_t end[3] = {0, 0, 0};
    const int32_t pitch[3] = {port.yPitch, port.chromaPitch(), port.chromaPitch()};
    size_t length = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0)
            offset[i] = port.planeNumber >= static_cast<uint32_t>(n)
                                ? offset[i - 1] + port.sizes[i - 1]
                                : end[i - 1];
        end[i] = offset[i] + static_cast<size_t>(pitch[i]) * (i ? chromaRows : port.height);
    }
    for (uint32_t i = 0; i < port.planeNumber && i < 3; i++) length += port.sizes[i];
    if (length < end[n - 1]) {
        ALOGE("fd %d: %zu bytes, %dx%d %#x at pitch %d/%d needs %zu", port.fd, length,
              port.width, port.height, port.format->color, port.yPitch, port.chromaPitch(),
              end[n - 1]);
        return DP_STATUS_INVALID_BUFFER;
    }
    mapping->reset(new Mapping(port.fd, length, write));
    uint8_t* base = (*mapping)->get();
    if (!base) return DP_STATUS_INVALID_BUFFER;
    for (int i = 0; i < n; i++) {
        planes->p[i] = base + offset[i];
        planes->stride[i] = pitch[i];
    }
    if (port.format->layout == Layout::YV12) {
        std::swap(planes->p[1], planes->p[2]);
        std::swap(planes->stride[1], planes->stride[2]);
    }
    return DP_STATUS_RETURN_SUCCESS;
}

// Moves planes to pixel (x, y); x and y are even for subsampled formats.
Planes offsetPlanes(const Planes& in, const Format* f, int x, int y) {
    Planes out = in;
    if (f->layout == Layout::RGB || f->layout == Layout::YUYV || f->layout == Layout::UYVY) {
        out.p[0] += y * in.stride[0] + x * f->bpp;
        return out;
    }
    out.p[0] += y * in.stride[0] + x;
    if (isPlanar(f)) {
        out.p[1] += y / 2 * in.stride[1] + x / 2;
        out.p[2] += y / 2 * in.stride[2] + x / 2;
    } else {
        out.p[1] += y / 2 * in.stride[1] + x;
    }
    return out;
}

// An I420 image in memory of its own.
struct I420Buffer {
    std::vector<uint8_t> data;
    Planes planes;

    void allocate(int w, int h) {
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        data.resize(static_cast<size_t>(w) * h + 2 * cw * ch);
        planes.p[0] = data.data();
        planes.p[1] = planes.p[0] + w * h;
        planes.p[2] = planes.p[1] + cw * ch;
        planes.stride[0] = w;
        planes.stride[1] = planes.stride[2] = cw;
    }
};

// libyuv's ARGB, B G R A in memory, to I420 with a matrix libyuv has no
// entry point for: BT.709, limited range.
void argbToI420Bt709(const uint8_t* argb, int argbStride, const Planes& dst, int w, int h) {
    // Q16 coefficients: Y = 16 + 219/255 (Kr R + Kg G + Kb B), U and V
    // 128 + 224/255 of (B - Y') / (2 (1 - Kb)) and (R - Y') / (2 (1 - Kr)).
    constexpr double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
    constexpr double ys = 219.0 / 255, cs = 224.0 / 255;
    constexpr int s = 1 << 16;
    constexpr int yr = kr * ys * s + 0.5, yg = kg * ys * s + 0.5, yb = kb * ys * s + 0.5;
    constexpr double ud = 2 * (1 - kb), vd = 2 * (1 - kr);
    constexpr int ur = -kr / ud * cs * s - 0.5, ug = -kg / ud * cs * s - 0.5;
    constexpr int ub = (1 - kb) / ud * cs * s + 0.5;
    constexpr int vr = (1 - kr) / vd * cs * s + 0.5, vg = -kg / vd * cs * s - 0.5;
    constexpr int vb = -kb / vd * cs * s - 0.5;

    for (int y = 0; y < h; y++) {
        const uint8_t* px = argb + y * argbStride;
        uint8_t* out = dst.p[0] + y * dst.stride[0];
        for (int x = 0; x < w; x++, px += 4)
            out[x] = ((16 << 16 | 1 << 15) + yr * px[2] + yg * px[1] + yb * px[0]) >> 16;
    }
    for (int y = 0; y < h; y += 2) {
        const uint8_t* r0 = argb + y * argbStride;
        const uint8_t* r1 = y + 1 < h ? r0 + argbStride : r0;
        uint8_t* u = dst.p[1] + y / 2 * dst.stride[1];
        uint8_t* v = dst.p[2] + y / 2 * dst.stride[2];
        for (int x = 0; x < w; x += 2) {
            const int x1 = x + 1 < w ? x + 1 : x;
            int c[3];
            for (int k = 0; k < 3; k++)
                c[k] = r0[4 * x + k] + r0[4 * x1 + k] + r1[4 * x + k] + r1[4 * x1 + k];
            // c holds 4 x B, G, R.
            u[x / 2] = ((128 << 18 | 1 << 17) + ur * c[2] + ug * c[1] + ub * c[0]) >> 18;
            v[x / 2] = ((128 << 18 | 1 << 17) + vr * c[2] + vg * c[1] + vb * c[0]) >> 18;
        }
    }
}

// The RGB source at (x, y), w x h, to libyuv ARGB.
int rgbToArgb(const Planes& src, DP_COLOR_ENUM color, uint8_t* argb, int argbStride, int w,
              int h) {
    const uint8_t* s = src.p[0];
    const int ss = src.stride[0];
    switch (color) {
        case DP_COLOR_BGRA8888:  // B G R A: libyuv ARGB
            return libyuv::ARGBCopy(s, ss, argb, argbStride, w, h);
        case DP_COLOR_RGBA8888:  // R G B A: libyuv ABGR
            return libyuv::ABGRToARGB(s, ss, argb, argbStride, w, h);
        case DP_COLOR_ARGB8888:  // A R G B: libyuv BGRA
            return libyuv::BGRAToARGB(s, ss, argb, argbStride, w, h);
        case DP_COLOR_ABGR8888:  // A B G R: libyuv RGBA
            return libyuv::RGBAToARGB(s, ss, argb, argbStride, w, h);
        case DP_COLOR_RGB888:  // R G B: libyuv RAW
            return libyuv::RAWToARGB(s, ss, argb, argbStride, w, h);
        case DP_COLOR_BGR888:  // B G R: libyuv RGB24
            return libyuv::RGB24ToARGB(s, ss, argb, argbStride, w, h);
        case DP_COLOR_RGB565:
            return libyuv::RGB565ToARGB(s, ss, argb, argbStride, w, h);
        default:
            return -1;
    }
}

int argbToI420(const uint8_t* argb, int argbStride, const Planes& dst, int w, int h,
               DP_PROFILE_ENUM profile) {
    const Planes& d = dst;
    switch (profile) {
        case DP_PROFILE_BT601:
            return libyuv::ARGBToI420(argb, argbStride, d.p[0], d.stride[0], d.p[1], d.stride[1],
                                      d.p[2], d.stride[2], w, h);
        case DP_PROFILE_JPEG:
            return libyuv::ARGBToJ420(argb, argbStride, d.p[0], d.stride[0], d.p[1], d.stride[1],
                                      d.p[2], d.stride[2], w, h);
        case DP_PROFILE_BT709:
            argbToI420Bt709(argb, argbStride, dst, w, h);
            return 0;
    }
    ALOGE("unknown profile %d", profile);
    return -1;
}

// The YUV source at (x, y), w x h, to I420.
int yuvToI420(const Planes& s, const Format* f, const Planes& d, int w, int h) {
    switch (f->layout) {
        case Layout::I420:
        case Layout::YV12:
            return libyuv::I420Copy(s.p[0], s.stride[0], s.p[1], s.stride[1], s.p[2],
                                    s.stride[2], d.p[0], d.stride[0], d.p[1], d.stride[1],
                                    d.p[2], d.stride[2], w, h);
        case Layout::NV12:
            return libyuv::NV12ToI420(s.p[0], s.stride[0], s.p[1], s.stride[1], d.p[0],
                                      d.stride[0], d.p[1], d.stride[1], d.p[2], d.stride[2], w,
                                      h);
        case Layout::NV21:
            return libyuv::NV21ToI420(s.p[0], s.stride[0], s.p[1], s.stride[1], d.p[0],
                                      d.stride[0], d.p[1], d.stride[1], d.p[2], d.stride[2], w,
                                      h);
        case Layout::YUYV:
            return libyuv::YUY2ToI420(s.p[0], s.stride[0], d.p[0], d.stride[0], d.p[1],
                                      d.stride[1], d.p[2], d.stride[2], w, h);
        case Layout::UYVY:
            return libyuv::UYVYToI420(s.p[0], s.stride[0], d.p[0], d.stride[0], d.p[1],
                                      d.stride[1], d.p[2], d.stride[2], w, h);
        case Layout::RGB:
            break;
    }
    return -1;
}

libyuv::RotationMode rotationMode(int32_t degrees) {
    switch (degrees) {
        case 90:
            return libyuv::kRotate90;
        case 180:
            return libyuv::kRotate180;
        case 270:
            return libyuv::kRotate270;
        default:
            return libyuv::kRotate0;
    }
}

bool validRotation(int32_t degrees) {
    return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

bool supportedDst(const Format* f) {
    return f && (isPlanar(f) || isSemiPlanar(f));
}

int evenDown(int v) {
    return v & ~1;
}

}  // namespace

struct DpAsyncBlitStream::Impl {
    std::mutex lock;
    Port src;
    Port dst;
    uint32_t nextJob = 1;
    int idleBuf = -1;  // a dma-buf no one fences, for signalled sync_files

    Impl() {
        int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        if (heap < 0) {
            ALOGE("open /dev/dma_heap/system: %s", strerror(errno));
            return;
        }
        struct dma_heap_allocation_data alloc = {
                .len = 4096,
                .fd = 0,
                .fd_flags = O_RDWR | O_CLOEXEC,
                .heap_flags = 0,
        };
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc))
            ALOGE("DMA_HEAP_IOCTL_ALLOC: %s", strerror(errno));
        else
            idleBuf = alloc.fd;
        close(heap);
    }
    ~Impl() {
        if (idleBuf >= 0) close(idleBuf);
    }

    int signalledFence() {
        if (idleBuf < 0) return -1;
        struct dma_buf_export_sync_file e = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
        if (ioctl(idleBuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e)) {
            ALOGE("DMA_BUF_IOCTL_EXPORT_SYNC_FILE: %s", strerror(errno));
            return -1;
        }
        return e.fd;
    }

    DP_STATUS_ENUM convert();
};

DP_STATUS_ENUM DpAsyncBlitStream::Impl::convert() {
    const Format* sf = src.format;
    const Format* df = dst.format;
    if (!sf || !supportedDst(df)) return DP_STATUS_INVALID_FORMAT;

    DpRect crop = src.hasRect ? src.rect : DpRect(0, 0, src.width, src.height);
    DpRect roi = dst.hasRect ? dst.rect : DpRect(0, 0, dst.width, dst.height);
    if (sf->layout != Layout::RGB) {
        crop.x = evenDown(crop.x);
        crop.y = evenDown(crop.y);
    }
    roi.x = evenDown(roi.x);
    roi.y = evenDown(roi.y);
    if (crop.w <= 0 || crop.h <= 0 || crop.x < 0 || crop.y < 0 ||
        crop.x + crop.w > src.width || crop.y + crop.h > src.height) {
        ALOGE("crop %d,%d %dx%d outside %dx%d", crop.x, crop.y, crop.w, crop.h, src.width,
              src.height);
        return DP_STATUS_INVALID_CROP;
    }
    if (roi.w <= 0 || roi.h <= 0 || roi.x < 0 || roi.y < 0 || roi.x + roi.w > dst.width ||
        roi.y + roi.h > dst.height) {
        ALOGE("roi %d,%d %dx%d outside %dx%d", roi.x, roi.y, roi.w, roi.h, dst.width,
              dst.height);
        return DP_STATUS_INVALID_CROP;
    }
    const libyuv::RotationMode rot = rotationMode(dst.rotation);
    const bool swapped = rot == libyuv::kRotate90 || rot == libyuv::kRotate270;
    // The size before rotation.
    const int sw = swapped ? roi.h : roi.w;
    const int sh = swapped ? roi.w : roi.h;
    const bool scale = crop.w != sw || crop.h != sh;

    std::unique_ptr<Mapping> srcMap, dstMap;
    Planes sp, dp;
    DP_STATUS_ENUM status = mapPort(src, false, &srcMap, &sp);
    if (status != DP_STATUS_RETURN_SUCCESS) return status;
    status = mapPort(dst, true, &dstMap, &dp);
    if (status != DP_STATUS_RETURN_SUCCESS) return status;
    sp = offsetPlanes(sp, sf, crop.x, crop.y);
    dp = offsetPlanes(dp, df, roi.x, roi.y);

    // Into I420 of sw x sh: straight into the destination when it is planar
    // and nothing is rotated.
    I420Buffer stageBuf;
    Planes stage;
    if (rot == libyuv::kRotate0 && isPlanar(df)) {
        stage = dp;
    } else {
        stageBuf.allocate(sw, sh);
        stage = stageBuf.planes;
    }

    int ret;
    if (sf->layout == Layout::RGB) {
        std::vector<uint8_t> argb(static_cast<size_t>(crop.w) * crop.h * 4);
        ret = rgbToArgb(sp, sf->color, argb.data(), crop.w * 4, crop.w, crop.h);
        if (!ret && scale) {
            std::vector<uint8_t> scaled(static_cast<size_t>(sw) * sh * 4);
            ret = libyuv::ARGBScale(argb.data(), crop.w * 4, crop.w, crop.h, scaled.data(),
                                    sw * 4, sw, sh, libyuv::kFilterBilinear);
            argb.swap(scaled);
        }
        if (!ret) ret = argbToI420(argb.data(), sw * 4, stage, sw, sh, dst.profile);
    } else if (!scale) {
        ret = yuvToI420(sp, sf, stage, sw, sh);
    } else {
        Planes in = sp;
        I420Buffer unscaled;
        ret = 0;
        if (!isPlanar(sf)) {
            unscaled.allocate(crop.w, crop.h);
            ret = yuvToI420(sp, sf, unscaled.planes, crop.w, crop.h);
            in = unscaled.planes;
        }
        if (!ret)
            ret = libyuv::I420Scale(in.p[0], in.stride[0], in.p[1], in.stride[1], in.p[2],
                                    in.stride[2], crop.w, crop.h, stage.p[0], stage.stride[0],
                                    stage.p[1], stage.stride[1], stage.p[2], stage.stride[2],
                                    sw, sh, libyuv::kFilterBilinear);
    }
    if (ret) {
        ALOGE("%#x %dx%d to I420 %dx%d failed", sf->color, crop.w, crop.h, sw, sh);
        return DP_STATUS_INVALID_FORMAT;
    }

    // Rotate, and interleave the chroma for NV12 and NV21.
    Planes out = stage;
    I420Buffer rotated;
    if (rot != libyuv::kRotate0) {
        if (isPlanar(df)) {
            out = dp;
        } else {
            rotated.allocate(roi.w, roi.h);
            out = rotated.planes;
        }
        ret = libyuv::I420Rotate(stage.p[0], stage.stride[0], stage.p[1], stage.stride[1],
                                 stage.p[2], stage.stride[2], out.p[0], out.stride[0], out.p[1],
                                 out.stride[1], out.p[2], out.stride[2], sw, sh, rot);
    }
    if (!ret && df->layout == Layout::NV12)
        ret = libyuv::I420ToNV12(out.p[0], out.stride[0], out.p[1], out.stride[1], out.p[2],
                                 out.stride[2], dp.p[0], dp.stride[0], dp.p[1], dp.stride[1],
                                 roi.w, roi.h);
    else if (!ret && df->layout == Layout::NV21)
        ret = libyuv::I420ToNV21(out.p[0], out.stride[0], out.p[1], out.stride[1], out.p[2],
                                 out.stride[2], dp.p[0], dp.stride[0], dp.p[1], dp.stride[1],
                                 roi.w, roi.h);
    if (ret) {
        ALOGE("I420 %dx%d to %#x, rotation %d, failed", roi.w, roi.h, df->color, dst.rotation);
        return DP_STATUS_INVALID_FORMAT;
    }
    return DP_STATUS_RETURN_SUCCESS;
}

DpAsyncBlitStream::DpAsyncBlitStream() : mImpl(new Impl) {}

DpAsyncBlitStream::~DpAsyncBlitStream() {
    delete mImpl;
}

bool DpAsyncBlitStream::queryHWSupport(uint32_t srcWidth, uint32_t srcHeight,
                                       uint32_t dstWidth, uint32_t dstHeight,
                                       int32_t orientation, DP_COLOR_ENUM srcFormat,
                                       DP_COLOR_ENUM dstFormat, DpPqParam*, DpRect*, uint32_t) {
    return srcWidth && srcHeight && dstWidth && dstHeight && validRotation(orientation) &&
           findFormat(srcFormat) && supportedDst(findFormat(dstFormat));
}

DP_STATUS_ENUM DpAsyncBlitStream::createJob(uint32_t& jobID, int32_t& fenceFD) {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    jobID = mImpl->nextJob++;
    fenceFD = mImpl->signalledFence();
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setConfigBegin(uint32_t) {
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setSrcBuffer(int32_t fileDesc, uint32_t* sizeList,
                                               uint32_t planeNumber, int32_t fenceFD) {
    if (fileDesc < 0 || !sizeList || !planeNumber || planeNumber > 3)
        return DP_STATUS_INVALID_BUFFER;
    if (fenceFD >= 0 && sync_wait(fenceFD, -1))
        ALOGW("source fence %d: %s", fenceFD, strerror(errno));
    std::lock_guard<std::mutex> guard(mImpl->lock);
    Port& p = mImpl->src;
    p.fd = fileDesc;
    p.planeNumber = planeNumber;
    memcpy(p.sizes, sizeList, planeNumber * sizeof(*sizeList));
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setSrcConfig(int32_t width, int32_t height, int32_t yPitch,
                                               int32_t uvPitch, DP_COLOR_ENUM format,
                                               DP_PROFILE_ENUM profile, DpInterlaceFormat,
                                               DpSecure secure, bool, uint32_t) {
    const Format* f = findFormat(format);
    if (!f) {
        ALOGE("source format %#x not supported", format);
        return DP_STATUS_INVALID_FORMAT;
    }
    if (secure != DP_SECURE_NONE) return DP_STATUS_INVALID_BUFFER;
    if (width <= 0) return DP_STATUS_INVALID_WIDTH;
    if (height <= 0 || yPitch < width * f->bpp) return DP_STATUS_INVALID_HEIGHT;
    std::lock_guard<std::mutex> guard(mImpl->lock);
    Port& p = mImpl->src;
    p.width = width;
    p.height = height;
    p.yPitch = yPitch;
    p.uvPitch = uvPitch;
    p.format = f;
    p.profile = profile;
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setSrcCrop(uint32_t portIndex, DpRect roi) {
    if (portIndex) return DP_STATUS_INVALID_PORT;
    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->src.rect = roi;
    mImpl->src.hasRect = roi.w > 0 && roi.h > 0;
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setDstBuffer(uint32_t portIndex, int32_t fileDesc,
                                               uint32_t* sizeList, uint32_t planeNumber,
                                               int32_t fenceFD) {
    if (portIndex) return DP_STATUS_INVALID_PORT;
    if (fileDesc < 0 || !sizeList || !planeNumber || planeNumber > 3)
        return DP_STATUS_INVALID_BUFFER;
    if (fenceFD >= 0 && sync_wait(fenceFD, -1))
        ALOGW("destination fence %d: %s", fenceFD, strerror(errno));
    std::lock_guard<std::mutex> guard(mImpl->lock);
    Port& p = mImpl->dst;
    p.fd = fileDesc;
    p.planeNumber = planeNumber;
    memcpy(p.sizes, sizeList, planeNumber * sizeof(*sizeList));
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setDstConfig(uint32_t portIndex, int32_t width,
                                               int32_t height, int32_t yPitch, int32_t uvPitch,
                                               DP_COLOR_ENUM format, DP_PROFILE_ENUM profile,
                                               DpInterlaceFormat, DpRect* roi, DpSecure secure,
                                               bool, uint32_t, int32_t) {
    if (portIndex) return DP_STATUS_INVALID_PORT;
    const Format* f = findFormat(format);
    if (!supportedDst(f)) {
        ALOGE("destination format %#x not supported", format);
        return DP_STATUS_INVALID_FORMAT;
    }
    if (secure != DP_SECURE_NONE) return DP_STATUS_INVALID_BUFFER;
    if (width <= 0) return DP_STATUS_INVALID_WIDTH;
    if (height <= 0 || yPitch < width) return DP_STATUS_INVALID_HEIGHT;
    std::lock_guard<std::mutex> guard(mImpl->lock);
    Port& p = mImpl->dst;
    p.width = width;
    p.height = height;
    p.yPitch = yPitch;
    p.uvPitch = uvPitch;
    p.format = f;
    p.profile = profile;
    p.hasRect = roi && roi->w > 0 && roi->h > 0;
    if (p.hasRect) p.rect = *roi;
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setRotate(uint32_t portIndex, int32_t rotation) {
    if (portIndex) return DP_STATUS_INVALID_PORT;
    if (!validRotation(rotation)) return DP_STATUS_INVALID_ANGLE;
    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->dst.rotation = rotation;
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::setConfigEnd() {
    return DP_STATUS_RETURN_SUCCESS;
}

DP_STATUS_ENUM DpAsyncBlitStream::invalidate(struct timeval* endTime) {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    DP_STATUS_ENUM status = mImpl->convert();
    if (endTime) gettimeofday(endTime, nullptr);
    return status;
}

DpBlitStream::DpBlitStream() {}

DpBlitStream::~DpBlitStream() {}

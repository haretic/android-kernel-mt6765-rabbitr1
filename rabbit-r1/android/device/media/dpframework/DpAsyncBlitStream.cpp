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

enum class Layout { RGB, I420, YV12, NV12, NV21, YUYV, UYVY, BLK };

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
        {DP_COLOR_NV21, Layout::NV21, 1},    {DP_COLOR_420_BLK, Layout::BLK, 1},
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
    return isPlanar(f) ? 3 : isSemiPlanar(f) || f->layout == Layout::BLK ? 2 : 1;
}

int alignUp(int v, int a) {
    return (v + a - 1) & ~(a - 1);
}

// Rows of plane i in memory: block formats fill their last row of tiles,
// 32 rows of luma and 16 of chroma.
int planeRows(const Format* f, int plane, int height) {
    const int rows = plane ? (height + 1) / 2 : height;
    if (f->layout == Layout::BLK) return alignUp(rows, plane ? 16 : 32);
    return rows;
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
    size_t offset[3] = {0, 0, 0};
    size_t end[3] = {0, 0, 0};
    const int32_t pitch[3] = {port.yPitch, port.chromaPitch(), port.chromaPitch()};
    size_t length = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0)
            offset[i] = port.planeNumber >= static_cast<uint32_t>(n)
                                ? offset[i - 1] + port.sizes[i - 1]
                                : end[i - 1];
        end[i] = offset[i] + static_cast<size_t>(pitch[i]) * planeRows(port.format, i, port.height);
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

// Copies w x h of the source at its planes into cached memory. Buffers
// allocated without ION_FLAG_CACHED, as the PowerVR gralloc's render targets
// are, have write-combined CPU mappings, where the narrow loads of the
// conversion routines run at tens of MB/s; memcpy's wide loads do much better.
Planes copyToCached(const Planes& in, const Format* f, int w, int h,
                    std::vector<uint8_t>* store) {
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    int rowBytes[3] = {w * f->bpp, 0, 0};
    int rows[3] = {h, 0, 0};
    if (isPlanar(f)) {
        rowBytes[1] = rowBytes[2] = cw;
        rows[1] = rows[2] = ch;
    } else if (isSemiPlanar(f)) {
        rowBytes[1] = cw * 2;
        rows[1] = ch;
    }
    size_t total = 0;
    for (int i = 0; i < 3; i++) total += static_cast<size_t>(rowBytes[i]) * rows[i];
    store->resize(total);
    Planes out;
    uint8_t* dst = store->data();
    for (int i = 0; i < 3 && rows[i]; i++) {
        out.p[i] = dst;
        out.stride[i] = rowBytes[i];
        for (int y = 0; y < rows[i]; y++, dst += rowBytes[i])
            memcpy(dst, in.p[i] + static_cast<size_t>(y) * in.stride[i], rowBytes[i]);
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

// 8-bit fixed-point RGB to YUV coefficients, as in libyuv's rows: Y =
// (yr R + yg G + yb B + yBias) >> 8, U = (ub B - ur R - ug G + cBias) >> 8,
// V = (vr R - vg G - vb B + cBias) >> 8.
struct Matrix {
    uint16_t yr, yg, yb, yBias;
    uint16_t ur, ug, ub;
    uint16_t vr, vg, vb;
    uint16_t cBias;
};

constexpr uint16_t q8(double v) {
    return static_cast<uint16_t>(v * 256 + 0.5);
}

// Y = 16 + 219/255 (Kr R + Kg G + Kb B) for limited range, Kr R + Kg G + Kb B
// for full; U and V are 128 plus 224/255 (limited) or all of (B - Y') /
// (2 (1 - Kb)) and (R - Y') / (2 (1 - Kr)). The rounded coefficients are
// evened out so that Y's add up to its scale and U's and V's to 0: grey
// stays grey.
constexpr Matrix makeMatrix(double kr, double kb, bool full) {
    const double ys = full ? 1 : 219.0 / 255, cs = full ? 1 : 224.0 / 255;
    Matrix m{};
    m.yr = q8(kr * ys);
    m.yb = q8(kb * ys);
    m.yg = q8(ys) - m.yr - m.yb;
    m.yBias = full ? 0x80 : 0x1080;
    m.ub = q8(cs / 2);
    m.ur = q8(kr / (2 * (1 - kb)) * cs);
    m.ug = m.ub - m.ur;
    m.vr = q8(cs / 2);
    m.vb = q8(kb / (2 * (1 - kr)) * cs);
    m.vg = m.vr - m.vb;
    // 128 << 8 plus rounding; full range's 128 x 255 needs one short of
    // 0x80 for the sums to stay within 16 bits.
    m.cBias = full ? 0x807f : 0x8080;
    return m;
}

constexpr Matrix kBt709 = makeMatrix(0.2126, 0.0722, false);
constexpr Matrix kBt709Full = makeMatrix(0.2126, 0.0722, true);
constexpr Matrix kBt2020 = makeMatrix(0.2627, 0.0593, false);
constexpr Matrix kBt2020Full = makeMatrix(0.2627, 0.0593, true);

// libyuv's ARGB, B G R A in memory, to I420 with a matrix libyuv has no entry
// point for, in loops the compiler vectorises. Chroma comes from 2x2 averages.
// The sums wrap modulo 2^16 on the way but end in 0..255 after the bias.
void argbToI420Matrix(const uint8_t* argb, int argbStride, const Planes& dst, int w, int h,
                      const Matrix& m, std::vector<uint16_t>* scratch) {
    for (int y = 0; y < h; y++) {
        const uint8_t* px = argb + y * argbStride;
        uint8_t* out = dst.p[0] + y * dst.stride[0];
        for (int x = 0; x < w; x++)
            out[x] = static_cast<uint16_t>(m.yr * px[4 * x + 2] + m.yg * px[4 * x + 1] +
                                           m.yb * px[4 * x] + m.yBias) >> 8;
    }

    // Per row pair: the vertical sums of each channel, then the horizontal
    // pairs of those.
    const int cw = (w + 1) / 2;
    scratch->resize(3 * 2 * cw);
    uint16_t* sb = scratch->data();
    uint16_t* sg = sb + 2 * cw;
    uint16_t* sr = sg + 2 * cw;
    for (int y = 0; y < h; y += 2) {
        const uint8_t* r0 = argb + y * argbStride;
        const uint8_t* r1 = y + 1 < h ? r0 + argbStride : r0;
        for (int x = 0; x < w; x++) {
            sb[x] = r0[4 * x] + r1[4 * x];
            sg[x] = r0[4 * x + 1] + r1[4 * x + 1];
            sr[x] = r0[4 * x + 2] + r1[4 * x + 2];
        }
        if (w & 1) {
            sb[w] = sb[w - 1];
            sg[w] = sg[w - 1];
            sr[w] = sr[w - 1];
        }
        uint8_t* u = dst.p[1] + y / 2 * dst.stride[1];
        uint8_t* v = dst.p[2] + y / 2 * dst.stride[2];
        for (int i = 0; i < cw; i++) {
            const uint16_t b = (sb[2 * i] + sb[2 * i + 1] + 2) >> 2;
            const uint16_t g = (sg[2 * i] + sg[2 * i + 1] + 2) >> 2;
            const uint16_t r = (sr[2 * i] + sr[2 * i + 1] + 2) >> 2;
            u[i] = static_cast<uint16_t>(m.ub * b - m.ur * r - m.ug * g + m.cBias) >> 8;
            v[i] = static_cast<uint16_t>(m.vr * r - m.vg * g - m.vb * b + m.cBias) >> 8;
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
               DP_PROFILE_ENUM profile, std::vector<uint16_t>* scratch) {
    const Planes& d = dst;
    switch (profile) {
        case DP_PROFILE_BT601:
            return libyuv::ARGBToI420(argb, argbStride, d.p[0], d.stride[0], d.p[1], d.stride[1],
                                      d.p[2], d.stride[2], w, h);
        case DP_PROFILE_JPEG:
            return libyuv::ARGBToJ420(argb, argbStride, d.p[0], d.stride[0], d.p[1], d.stride[1],
                                      d.p[2], d.stride[2], w, h);
        case DP_PROFILE_BT709:
            argbToI420Matrix(argb, argbStride, dst, w, h, kBt709, scratch);
            return 0;
        case DP_PROFILE_FULL_BT709:
            argbToI420Matrix(argb, argbStride, dst, w, h, kBt709Full, scratch);
            return 0;
        case DP_PROFILE_BT2020:
            argbToI420Matrix(argb, argbStride, dst, w, h, kBt2020, scratch);
            return 0;
        case DP_PROFILE_FULL_BT2020:
            argbToI420Matrix(argb, argbStride, dst, w, h, kBt2020Full, scratch);
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
        case Layout::BLK:
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

    // Kept from job to job: allocating and clearing them each frame costs
    // as much as some of the conversions.
    std::vector<uint8_t> srcCopy;
    std::vector<uint8_t> detiled;
    std::vector<uint8_t> argb;
    std::vector<uint8_t> scaled;
    std::vector<uint16_t> rows;
    I420Buffer stageBuf;
    I420Buffer unscaled;
    I420Buffer rotated;

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
    if (sf->layout == Layout::BLK) {
        // Tiles do not crop: detile down to the crop's bottom right corner
        // into linear NV12, which is in cached memory then.
        const int w = crop.x + crop.w, h = alignUp(crop.y + crop.h, 2);
        const int stride = alignUp(w, 2);
        detiled.resize(static_cast<size_t>(stride) * h * 3 / 2);
        Planes lin;
        lin.p[0] = detiled.data();
        lin.p[1] = lin.p[0] + static_cast<size_t>(stride) * h;
        lin.stride[0] = lin.stride[1] = stride;
        if (libyuv::MM21ToNV12(sp.p[0], sp.stride[0], sp.p[1], sp.stride[1], lin.p[0],
                               lin.stride[0], lin.p[1], lin.stride[1], w, h)) {
            ALOGE("detiling %dx%d failed", w, h);
            return DP_STATUS_INVALID_FORMAT;
        }
        sf = findFormat(DP_COLOR_NV12);
        sp = offsetPlanes(lin, sf, crop.x, crop.y);
    } else {
        sp = offsetPlanes(sp, sf, crop.x, crop.y);
        sp = copyToCached(sp, sf, crop.w, crop.h, &srcCopy);
    }
    dp = offsetPlanes(dp, df, roi.x, roi.y);

    // Into I420 of sw x sh: straight into the destination when it is planar
    // and nothing is rotated.
    Planes stage;
    if (rot == libyuv::kRotate0 && isPlanar(df)) {
        stage = dp;
    } else {
        stageBuf.allocate(sw, sh);
        stage = stageBuf.planes;
    }

    int ret;
    if (sf->layout == Layout::RGB) {
        argb.resize(static_cast<size_t>(crop.w) * crop.h * 4);
        ret = rgbToArgb(sp, sf->color, argb.data(), crop.w * 4, crop.w, crop.h);
        if (!ret && scale) {
            scaled.resize(static_cast<size_t>(sw) * sh * 4);
            ret = libyuv::ARGBScale(argb.data(), crop.w * 4, crop.w, crop.h, scaled.data(),
                                    sw * 4, sw, sh, libyuv::kFilterBilinear);
        }
        const uint8_t* in = scale ? scaled.data() : argb.data();
        if (!ret) ret = argbToI420(in, sw * 4, stage, sw, sh, dst.profile, &rows);
    } else if (!scale) {
        ret = yuvToI420(sp, sf, stage, sw, sh);
    } else {
        Planes in = sp;
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
    if (f->layout == Layout::BLK && (yPitch % 16 || (uvPitch && uvPitch != yPitch))) {
        ALOGE("block format at pitch %d/%d", yPitch, uvPitch);
        return DP_STATUS_INVALID_PARAX;
    }
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

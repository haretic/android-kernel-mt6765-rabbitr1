// SPDX-License-Identifier: Apache-2.0
// The MediaTek DpFramework types that the stock Codec2 libraries pass across
// the libdpframework interface. Names, values and layouts are the interface's
// (the mangled symbol names and the callers' argument setup depend on them).
#ifndef R1_DP_DATA_TYPE_H
#define R1_DP_DATA_TYPE_H

#include <stdint.h>

typedef enum DP_STATUS_ENUM {
    DP_STATUS_RETURN_SUCCESS = 0,
    DP_STATUS_INVALID_PARAX = -1,
    DP_STATUS_INVALID_PORT = -2,
    DP_STATUS_INVALID_BUFFER = -6,
    DP_STATUS_INVALID_STATE = -7,
    DP_STATUS_INVALID_FORMAT = -9,
    DP_STATUS_INVALID_WIDTH = -16,
    DP_STATUS_INVALID_HEIGHT = -17,
    DP_STATUS_INVALID_CROP = -18,
    DP_STATUS_INVALID_ANGLE = -19,
    DP_STATUS_OUT_OF_MEMORY = -23,
} DP_STATUS_ENUM;

// libcodec2_mtk_venc picks these from the C2 colour aspects: BT.601, BT.709 or
// BT.2020 primaries, limited or full range (Aperture records full-range BT.709).
typedef enum DP_PROFILE_ENUM : int {
    DP_PROFILE_BT601,  // limited range
    DP_PROFILE_BT709,  // limited range
    DP_PROFILE_JPEG,   // BT.601, full range
    DP_PROFILE_FULL_BT601 = DP_PROFILE_JPEG,
    DP_PROFILE_BT2020,  // limited range
    DP_PROFILE_FULL_BT709,
    DP_PROFILE_FULL_BT2020,
} DP_PROFILE_ENUM;

enum DpInterlaceFormat {
    eInterlace_None,
    eTop_Field,
    eBottom_Field,
};

enum DpSecure {
    DP_SECURE_NONE = 0,
    DP_SECURE = 1,
    DP_SECURE_SHIFT = 8,
};

// Passed by value, which for this class (user-provided copy constructor and
// destructor) means by reference in the ABI; keep it non-trivial.
class DpRect {
  public:
    DpRect() : x(0), sub_x(0), y(0), sub_y(0), w(0), h(0) {}
    DpRect(int32_t in_x, int32_t in_y, int32_t in_w, int32_t in_h, int32_t in_sub_x = 0,
           int32_t in_sub_y = 0)
        : x(in_x), sub_x(in_sub_x), y(in_y), sub_y(in_sub_y), w(in_w), h(in_h) {}
    DpRect(const DpRect& rt)
        : x(rt.x), sub_x(rt.sub_x), y(rt.y), sub_y(rt.sub_y), w(rt.w), h(rt.h) {}
    ~DpRect() {}
    DpRect& operator=(const DpRect& rt) {
        x = rt.x;
        sub_x = rt.sub_x;
        y = rt.y;
        sub_y = rt.sub_y;
        w = rt.w;
        h = rt.h;
        return *this;
    }

    int32_t x;
    int32_t sub_x;
    int32_t y;
    int32_t sub_y;
    int32_t w;
    int32_t h;
};

// Only passed by pointer; the contents are not used.
struct DpPqParam;

// Callers pass formats beyond these, so the enums have a fixed type (the
// values stay ints in the ABI). The layout is the one of MDP_COLOR in the
// kernel's mtk-mdp3-regs.h, which the stock codec libraries use (their I420 is
// 0x6c0848). Group 0 is RGB, 1 YUV; SWAP exchanges R and B, or U and V. The
// names give the byte order in memory: RGBA8888 is R, G, B, A.
#define DP_COLORFMT_PACK(COMPRESS, PACKED, LOOSE, VIDEO, PLANE, HF, VF, BITS, GROUP, SWAP, ID) \
    (((COMPRESS) << 29) | ((PACKED) << 27) | ((LOOSE) << 26) | ((VIDEO) << 23) |             \
     ((PLANE) << 21) | ((HF) << 19) | ((VF) << 18) | ((BITS) << 8) | ((GROUP) << 6) |       \
     ((SWAP) << 5) | ((ID) << 0))

typedef enum DP_COLOR_ENUM : int {
    DP_COLOR_UNKNOWN = 0,
    DP_COLOR_RGB565 = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 0, 0, 16, 0, 0, 0),
    DP_COLOR_RGB888 = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 0, 0, 24, 0, 1, 1),
    DP_COLOR_BGR888 = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 0, 0, 24, 0, 0, 1),
    DP_COLOR_RGBA8888 = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 0, 0, 32, 0, 1, 2),
    DP_COLOR_BGRA8888 = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 0, 0, 32, 0, 0, 2),
    DP_COLOR_ARGB8888 = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 0, 0, 32, 0, 1, 3),
    DP_COLOR_ABGR8888 = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 0, 0, 32, 0, 0, 3),
    DP_COLOR_UYVY = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 1, 0, 16, 1, 0, 4),
    DP_COLOR_YUYV = DP_COLORFMT_PACK(0, 0, 0, 0, 1, 1, 0, 16, 1, 0, 5),
    DP_COLOR_I420 = DP_COLORFMT_PACK(0, 0, 0, 0, 3, 1, 1, 8, 1, 0, 8),
    DP_COLOR_YV12 = DP_COLORFMT_PACK(0, 0, 0, 0, 3, 1, 1, 8, 1, 1, 8),
    DP_COLOR_NV12 = DP_COLORFMT_PACK(0, 0, 0, 0, 2, 1, 1, 8, 1, 0, 12),
    DP_COLOR_NV21 = DP_COLORFMT_PACK(0, 0, 0, 0, 2, 1, 1, 8, 1, 1, 12),
} DP_COLOR_ENUM;

#endif  // R1_DP_DATA_TYPE_H

// SPDX-License-Identifier: Apache-2.0
// The DpAsyncBlitStream and DpBlitStream calls the stock Codec2 libraries
// make. Not virtual: the callers call the exported functions directly and
// only allocate the objects, so the state lives behind a pointer.
#ifndef R1_DP_ASYNC_BLIT_STREAM_H
#define R1_DP_ASYNC_BLIT_STREAM_H

#include <sys/time.h>

#include "DpDataType.h"

class DpAsyncBlitStream {
  public:
    DpAsyncBlitStream();
    ~DpAsyncBlitStream();

    static bool queryHWSupport(uint32_t srcWidth, uint32_t srcHeight, uint32_t dstWidth,
                               uint32_t dstHeight, int32_t orientation, DP_COLOR_ENUM srcFormat,
                               DP_COLOR_ENUM dstFormat, DpPqParam* pqParam, DpRect* srcCrop,
                               uint32_t flags);

    DP_STATUS_ENUM createJob(uint32_t& jobID, int32_t& fenceFD);
    DP_STATUS_ENUM setConfigBegin(uint32_t jobID);
    DP_STATUS_ENUM setSrcBuffer(int32_t fileDesc, uint32_t* sizeList, uint32_t planeNumber,
                                int32_t fenceFD);
    DP_STATUS_ENUM setSrcConfig(int32_t width, int32_t height, int32_t yPitch, int32_t uvPitch,
                                DP_COLOR_ENUM format, DP_PROFILE_ENUM profile,
                                DpInterlaceFormat field, DpSecure secure, bool doFlush,
                                uint32_t compress);
    DP_STATUS_ENUM setSrcCrop(uint32_t portIndex, DpRect roi);
    DP_STATUS_ENUM setDstBuffer(uint32_t portIndex, int32_t fileDesc, uint32_t* sizeList,
                                uint32_t planeNumber, int32_t fenceFD);
    DP_STATUS_ENUM setDstConfig(uint32_t portIndex, int32_t width, int32_t height,
                                int32_t yPitch, int32_t uvPitch, DP_COLOR_ENUM format,
                                DP_PROFILE_ENUM profile, DpInterlaceFormat field, DpRect* roi,
                                DpSecure secure, bool doFlush, uint32_t pqID, int32_t flags);
    DP_STATUS_ENUM setRotate(uint32_t portIndex, int32_t rotation);
    DP_STATUS_ENUM setConfigEnd();
    DP_STATUS_ENUM invalidate(struct timeval* endTime);

    struct Impl;

  private:
    Impl* mImpl;
};

// Constructed and destroyed by the libraries, never used otherwise.
class DpBlitStream {
  public:
    DpBlitStream();
    ~DpBlitStream();
};

#endif  // R1_DP_ASYNC_BLIT_STREAM_H

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mainline overrides applied after config_kernel_user_mt6765.h.
 *
 * Only options with no mainline counterpart are removed here. None of them
 * changes a bridge table, an ioctl, a structure shared with userspace or the
 * RGX_BUILD_OPTIONS_KM word checked against the userspace libraries.
 */

/* Mainline has no ION; dma-buf import goes through the dmabuf bridge. */
#undef SUPPORT_ION
#undef PVR_ANDROID_ION_HEADER
#undef PVR_ANDROID_ION_PRIV_HEADER
/*
 * The stock build read sg->length for ION buffers. Mainline exporters
 * (DMA-BUF heaps) DMA-map the table for the importer, so sg_dma_len()
 * is the length that matches sg_dma_address().
 */
#undef PVR_ANDROID_ION_USE_SG_LENGTH
#undef PVRSRV_ENABLE_PVR_ION_STATS
#undef ION_DEFAULT_HEAP_NAME
#undef ION_DEFAULT_HEAP_ID_MASK

/* There is no linux/sync.h (Android staging sync) in mainline. */
#undef PVR_ANDROID_SYNC_HEADER

/*
 * strlcpy() was removed from the kernel (6.8). The DDK relies on its return
 * value (length of the source string), so keep an exact local copy rather
 * than mapping it onto strscpy(). Declared with compiler types because this
 * header is force-included before any kernel header.
 */
__SIZE_TYPE__ pvr_strlcpy(char *dst, const char *src, __SIZE_TYPE__ size);
#define strlcpy(dst, src, size) pvr_strlcpy(dst, src, size)

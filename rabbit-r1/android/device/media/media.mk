# SPDX-License-Identifier: Apache-2.0
# MT6765 hardware video codec: the stock MediaTek Codec2 HAL on the
# mainline /dev/Vcodec driver (drivers/staging/mtk-vcodec-compat). See
# README.md for the hookup and the staged bring-up.
#
# Blobs: vendor/rabbit/r1-media/proprietary/ holds the files in blobs.txt,
# copied from out/stock-vendor/tree and patched as blobs.txt describes:
# the shim NEEDED, every ELF's DT_RUNPATH, and the Android 12 libraries whose
# names AOSP modules own moved to /vendor/lib/r1-c2.
# Board side: BoardConfigMedia.mk (sepolicy).

ifneq ($(R1_GPU),pvr)
$(error media/media.mk: the stock codec stack maps buffers with the PowerVR gralloc, set R1_GPU=pvr)
endif

R1_MEDIA_PATH := device/rabbit/r1/media
R1_MEDIA_BLOBS := vendor/rabbit/r1-media/proprietary

# Generated from blobs.txt; keep the two in sync.
PRODUCT_COPY_FILES += \
    $(R1_MEDIA_BLOBS)/etc/mtk_platform_codecs_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mtk_platform_codecs_config.xml \
    $(R1_MEDIA_PATH)/seccomp_policy/android.hardware.media.c2@1.2-extended-seccomp-policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/android.hardware.media.c2@1.2-extended-seccomp-policy \
    $(R1_MEDIA_BLOBS)/etc/seccomp_policy/android.hardware.media.c2@1.2-mediatek-seccomp-policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/android.hardware.media.c2@1.2-mediatek-seccomp-policy \
    $(R1_MEDIA_BLOBS)/bin/hw/android.hardware.media.c2@1.2-mediatek:$(TARGET_COPY_OUT_VENDOR)/bin/hw/android.hardware.media.c2@1.2-mediatek \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/android.hardware.media.c2@1.0.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/android.hardware.media.c2@1.0.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/android.hardware.media.c2@1.1.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/android.hardware.media.c2@1.1.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/android.hardware.media.c2@1.2.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/android.hardware.media.c2@1.2.so \
    $(R1_MEDIA_BLOBS)/lib/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so:$(TARGET_COPY_OUT_VENDOR)/lib/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so \
    $(R1_MEDIA_BLOBS)/lib/hw/gralloc.rogue.so:$(TARGET_COPY_OUT_VENDOR)/lib/hw/gralloc.rogue.so \
    $(R1_MEDIA_BLOBS)/lib/libHEVCdec_sa.ca7.android.so:$(TARGET_COPY_OUT_VENDOR)/lib/libHEVCdec_sa.ca7.android.so \
    $(R1_MEDIA_BLOBS)/lib/libPVRMtkutils.so:$(TARGET_COPY_OUT_VENDOR)/lib/libPVRMtkutils.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libavservices_minijail_vendor.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libavservices_minijail_vendor.so \
    $(R1_MEDIA_BLOBS)/lib/libbwc.so:$(TARGET_COPY_OUT_VENDOR)/lib/libbwc.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libcodec2_hidl@1.0.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libcodec2_hidl@1.0.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libcodec2_hidl@1.1.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libcodec2_hidl@1.1.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libcodec2_hidl@1.2.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libcodec2_hidl@1.2.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libcodec2_hidl_plugin.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libcodec2_hidl_plugin.so \
    $(R1_MEDIA_BLOBS)/lib/libcodec2_mtk_c2store.so:$(TARGET_COPY_OUT_VENDOR)/lib/libcodec2_mtk_c2store.so \
    $(R1_MEDIA_BLOBS)/lib/libcodec2_mtk_vdec.so:$(TARGET_COPY_OUT_VENDOR)/lib/libcodec2_mtk_vdec.so \
    $(R1_MEDIA_BLOBS)/lib/libcodec2_mtk_venc.so:$(TARGET_COPY_OUT_VENDOR)/lib/libcodec2_mtk_venc.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libcodec2_soft_common.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libcodec2_soft_common.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libcodec2_vndk.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libcodec2_vndk.so \
    $(R1_MEDIA_BLOBS)/lib/libcodec2_vpp_qt_plugin.so:$(TARGET_COPY_OUT_VENDOR)/lib/libcodec2_vpp_qt_plugin.so \
    $(R1_MEDIA_BLOBS)/lib/libcodec2_vpp_rs_plugin.so:$(TARGET_COPY_OUT_VENDOR)/lib/libcodec2_vpp_rs_plugin.so \
    $(R1_MEDIA_BLOBS)/lib/libdpframework.so:$(TARGET_COPY_OUT_VENDOR)/lib/libdpframework.so \
    $(R1_MEDIA_BLOBS)/lib/libformatter.so:$(TARGET_COPY_OUT_VENDOR)/lib/libformatter.so \
    $(R1_MEDIA_BLOBS)/lib/libged.so:$(TARGET_COPY_OUT_VENDOR)/lib/libged.so \
    $(R1_MEDIA_BLOBS)/lib/libgpud.so:$(TARGET_COPY_OUT_VENDOR)/lib/libgpud.so \
    $(R1_MEDIA_BLOBS)/lib/libgralloc_extra.so:$(TARGET_COPY_OUT_VENDOR)/lib/libgralloc_extra.so \
    $(R1_MEDIA_BLOBS)/lib/libgralloc_metadata.so:$(TARGET_COPY_OUT_VENDOR)/lib/libgralloc_metadata.so \
    $(R1_MEDIA_BLOBS)/lib/libh264dec_customize.so:$(TARGET_COPY_OUT_VENDOR)/lib/libh264dec_customize.so \
    $(R1_MEDIA_BLOBS)/lib/libh264dec_sa.ca7.so:$(TARGET_COPY_OUT_VENDOR)/lib/libh264dec_sa.ca7.so \
    $(R1_MEDIA_BLOBS)/lib/libh264dec_sd.ca7.so:$(TARGET_COPY_OUT_VENDOR)/lib/libh264dec_sd.ca7.so \
    $(R1_MEDIA_BLOBS)/lib/libh264dec_se.ca7.so:$(TARGET_COPY_OUT_VENDOR)/lib/libh264dec_se.ca7.so \
    $(R1_MEDIA_BLOBS)/lib/libh264enc_sa.ca7.so:$(TARGET_COPY_OUT_VENDOR)/lib/libh264enc_sa.ca7.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libhwbinder.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libhwbinder.so \
    $(R1_MEDIA_BLOBS)/lib/libion_mtk.so:$(TARGET_COPY_OUT_VENDOR)/lib/libion_mtk.so \
    $(R1_MEDIA_BLOBS)/lib/libion_ulit.so:$(TARGET_COPY_OUT_VENDOR)/lib/libion_ulit.so \
    $(R1_MEDIA_BLOBS)/lib/libladder.so:$(TARGET_COPY_OUT_VENDOR)/lib/libladder.so \
    $(R1_MEDIA_BLOBS)/lib/libmp4enc_sa.ca7.so:$(TARGET_COPY_OUT_VENDOR)/lib/libmp4enc_sa.ca7.so \
    $(R1_MEDIA_BLOBS)/lib/libmtk_drvb.so:$(TARGET_COPY_OUT_VENDOR)/lib/libmtk_drvb.so \
    $(R1_MEDIA_BLOBS)/lib/libmtkperf_client_vendor.so:$(TARGET_COPY_OUT_VENDOR)/lib/libmtkperf_client_vendor.so \
    $(R1_MEDIA_BLOBS)/lib/libpowerhalwrap_vendor.so:$(TARGET_COPY_OUT_VENDOR)/lib/libpowerhalwrap_vendor.so \
    $(R1_MEDIA_BLOBS)/lib/libpq_prot.so:$(TARGET_COPY_OUT_VENDOR)/lib/libpq_prot.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libsfplugin_ccodec_utils.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libsfplugin_ccodec_utils.so \
    $(R1_MEDIA_BLOBS)/lib/libsrv_um.so:$(TARGET_COPY_OUT_VENDOR)/lib/libsrv_um.so \
    $(R1_MEDIA_BLOBS)/lib/r1-c2/libstagefright_bufferpool@2.0.1.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libstagefright_bufferpool@2.0.1.so \
    $(R1_MEDIA_BLOBS)/lib/libthha.so:$(TARGET_COPY_OUT_VENDOR)/lib/libthha.so \
    $(R1_MEDIA_BLOBS)/lib/libvcodec_oal.so:$(TARGET_COPY_OUT_VENDOR)/lib/libvcodec_oal.so \
    $(R1_MEDIA_BLOBS)/lib/libvcodec_utility.so:$(TARGET_COPY_OUT_VENDOR)/lib/libvcodec_utility.so \
    $(R1_MEDIA_BLOBS)/lib/libvcodecdrv.so:$(TARGET_COPY_OUT_VENDOR)/lib/libvcodecdrv.so \
    $(R1_MEDIA_BLOBS)/lib/libvp8dec_sa.ca7.so:$(TARGET_COPY_OUT_VENDOR)/lib/libvp8dec_sa.ca7.so \
    $(R1_MEDIA_BLOBS)/lib/libvp9dec_sa.ca7.so:$(TARGET_COPY_OUT_VENDOR)/lib/libvp9dec_sa.ca7.so \
    $(R1_MEDIA_BLOBS)/lib/vendor.mediatek.hardware.mtkpower@1.0.so:$(TARGET_COPY_OUT_VENDOR)/lib/vendor.mediatek.hardware.mtkpower@1.0.so \
    $(R1_MEDIA_BLOBS)/lib/vendor.mediatek.hardware.mtkpower@1.1.so:$(TARGET_COPY_OUT_VENDOR)/lib/vendor.mediatek.hardware.mtkpower@1.1.so \
    $(R1_MEDIA_BLOBS)/lib/vendor.mediatek.hardware.mtkpower@1.2.so:$(TARGET_COPY_OUT_VENDOR)/lib/vendor.mediatek.hardware.mtkpower@1.2.so \
    $(R1_MEDIA_BLOBS)/lib/vendor.mediatek.hardware.pq@2.0.so:$(TARGET_COPY_OUT_VENDOR)/lib/vendor.mediatek.hardware.pq@2.0.so

# The Android 12 symbols the blobs import (shim/).
PRODUCT_PACKAGES += libr1_codec2_shim

# Platform libraries the blobs link against, as 32-bit vendor variants.
PRODUCT_PACKAGES += \
    android.hardware.graphics.bufferqueue@1.0.vendor:32 \
    android.hardware.graphics.bufferqueue@2.0.vendor:32 \
    android.hardware.graphics.common@1.2.vendor:32 \
    android.hardware.graphics.mapper@4.0.vendor:32 \
    android.hardware.media.bufferpool@2.0.vendor:32 \
    android.hardware.media.omx@1.0.vendor:32 \
    android.hardware.media@1.0.vendor:32 \
    android.hidl.safe_union@1.0.vendor:32 \
    libbase.vendor:32 \
    libbinder.vendor:32 \
    libc++.vendor:32 \
    libcodec2.vendor:32 \
    libcutils.vendor:32 \
    libdmabufheap.vendor:32 \
    libdrm.vendor:32 \
    libexpat.vendor:32 \
    libfmq.vendor:32 \
    libgralloctypes.vendor:32 \
    libhardware.vendor:32 \
    libhidlbase.vendor:32 \
    libhidlmemory.vendor:32 \
    libion.vendor:32 \
    libminijail.vendor:32 \
    libstagefright_bufferqueue_helper.vendor:32 \
    libstagefright_foundation.vendor:32 \
    libui.vendor:32 \
    libunwindstack.vendor:32 \
    libutils.vendor:32 \
    libutilscallstack.vendor:32

# The blobs' handlers subclass AHandler with the Android 12 layout. Android 17
# appended members (mLock and the delivery status) that the subclasses'
# fields overlay, so the Android 17 library locks garbage and spins. The
# HAL gets the Android 12 build (VNDK v31) through its runpath; the
# libraries are loaded once per name, so the Android 17 vendor libraries in
# the process share it. Their AHandler only appends to the Android 12
# prefix, which the Android 12 code alone touches.
PRODUCT_COPY_FILES += \
    prebuilts/vndk/v31/arm/arch-arm-armv7-a-neon/shared/vndk-core/libstagefright_foundation.so:$(TARGET_COPY_OUT_VENDOR)/lib/r1-c2/libstagefright_foundation.so

# HAL service, device node, codec list.
PRODUCT_COPY_FILES += \
    $(R1_MEDIA_PATH)/init.r1.media.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.media.rc \
    $(R1_MEDIA_PATH)/ueventd.r1-media.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.r1-media.rc

# The framework only lists HAL components named in a codec list. Stage 1
# lists the hardware H.264 encoder; stage 2 adds the H.264 and HEVC decoders.
# Both stay off until the kernel can serve them (README.md, "Stages"): with
# no list the HAL runs but the c2.android software codecs do all the work.
R1_MEDIA_HW_ENCODE ?= false
R1_MEDIA_HW_DECODE ?= false
ifeq ($(R1_MEDIA_HW_DECODE),true)
PRODUCT_COPY_FILES += \
    $(R1_MEDIA_PATH)/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml
else ifeq ($(R1_MEDIA_HW_ENCODE),true)
PRODUCT_COPY_FILES += \
    $(R1_MEDIA_PATH)/media_codecs_c2_encode.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml
endif

DEVICE_MANIFEST_FILE += $(R1_MEDIA_PATH)/manifest-media.xml

# The stock HAL is HIDL only. mainline common defaults the Codec2 HAL
# selection to aidl, which would hide it (the swcodec APEX serves both).
PRODUCT_VENDOR_PROPERTIES += \
    media.c2.hal.selection=hidl

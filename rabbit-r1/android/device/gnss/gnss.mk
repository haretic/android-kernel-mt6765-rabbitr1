# SPDX-License-Identifier: Apache-2.0
# MT6765 CONSYS GPS and the stock MediaTek GNSS userspace (mnld and the
# android.hardware.gnss-service.mediatek HAL) on the mainline kernel's
# gps_drv.ko. See README.md.
#
# Needs modem/modem.mk (libnvram.so, libhidltransport, hwservicemanager)
# and connectivity/connectivity.mk (wmt_drv, vendor.connsys.driver.ready).
#
# Blobs: vendor/rabbit/r1-gnss/proprietary/ holds the files in blobs.txt,
# copied verbatim from out/stock-vendor/tree.
# Board side: BoardConfigGnss.mk (sepolicy).

R1_GNSS_PATH := device/rabbit/r1/gnss
R1_GNSS_BLOBS := vendor/rabbit/r1-gnss/proprietary

# Generated from blobs.txt; keep the two in sync (two libraries are renamed, see there).
PRODUCT_COPY_FILES += \
    $(R1_GNSS_BLOBS)/bin/mnld:$(TARGET_COPY_OUT_VENDOR)/bin/mnld \
    $(R1_GNSS_BLOBS)/bin/hw/android.hardware.gnss-service.mediatek:$(TARGET_COPY_OUT_VENDOR)/bin/hw/android.hardware.gnss-service.mediatek \
    $(R1_GNSS_BLOBS)/etc/MNL_Config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/MNL_Config.xml \
    $(R1_GNSS_BLOBS)/lib64/hw/android.hardware.gnss-impl-mediatek.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/android.hardware.gnss-impl-mediatek.so \
    $(R1_GNSS_BLOBS)/lib64/hw/android.hardware.gnss@2.1-impl-mediatek.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/android.hardware.gnss@2.1-impl-mediatek.so \
    $(R1_GNSS_BLOBS)/lib64/hw/gps.default.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/gps.default.so \
    $(R1_GNSS_BLOBS)/lib64/libDR.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libDR.so \
    $(R1_GNSS_BLOBS)/lib64/libmtkmnl.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkmnl.so \
    $(R1_GNSS_BLOBS)/lib64/libmtksensorndkbridge.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtksensorndkbridge.so

# The HAL links the AIDL IGnss V1 stubs under their Android 12 name, which
# lived in the VNDK. The VNDK 31 snapshot carries that exact library.
PRODUCT_COPY_FILES += \
    prebuilts/vndk/v31/arm64/arch-arm64-armv8-a/shared/vndk-core/android.hardware.gnss-V1-ndk_platform.so:$(TARGET_COPY_OUT_VENDOR)/lib64/android.hardware.gnss-V1-ndk_platform.so

# AOSP libraries the blobs link against, as vendor variants (VNDK in
# Android 12). The stock copies are not shipped so that they match our
# libhidlbase and libcrypto.
PRODUCT_PACKAGES += \
    android.hardware.gnss@1.0.vendor \
    android.hardware.gnss@1.1.vendor \
    android.hardware.gnss@2.0.vendor \
    android.hardware.gnss@2.1.vendor \
    android.hardware.gnss.measurement_corrections@1.0.vendor \
    android.hardware.gnss.measurement_corrections@1.1.vendor \
    android.hardware.gnss.visibility_control@1.0.vendor \
    android.hardware.sensors@1.0.vendor \
    android.frameworks.sensorservice@1.0.vendor \
    libcurl.vendor \
    libssl.vendor

# Kernel module, services, device nodes.
PRODUCT_COPY_FILES += \
    $(R1_GNSS_PATH)/init.r1.gnss.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.gnss.rc \
    $(R1_GNSS_PATH)/ueventd.r1-gnss.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.r1-gnss.rc

DEVICE_MANIFEST_FILE += $(R1_GNSS_PATH)/manifest-gnss.xml

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml

# Stock wmt_loader publishes the CONSYS chip id; mnld picks its chip
# configuration from it. ro.vendor.mtk_log_hide_gps is the stock value
# (mnld masks positions in its logs).
PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.connsys.chipid=0x6765 \
    ro.vendor.mtk_log_hide_gps=1

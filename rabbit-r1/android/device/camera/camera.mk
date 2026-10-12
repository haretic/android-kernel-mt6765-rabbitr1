#
# SPDX-License-Identifier: Apache-2.0
#
# Hi-846 rear camera through libcamera on the mainline kernel: include from
# device.mk, and include BoardConfigCamera.mk from the device BoardConfig.mk.
#
# Capture path: hi846 -> mtk-seninf -> CAMSV1 video node (MTISP packed
# 10-bit Bayer), matched by the libcamera simple pipeline handler with the
# CPU software ISP. CAMSV has no IOMMU and can't address above 4 GiB, so
# the raw buffers are MMAP buffers from the camsv1 CMA pool; the software
# ISP writes its output to gralloc or DMA heap buffers with the CPU.
#
# Don't set TARGET_CAMERA_PROVIDER_HAL=libcamera from mainline common
# together with this file, it points to a libcamera APEX that doesn't exist.
#

R1_CAMERA_PATH := device/rabbit/r1/camera

# AIDL camera provider loading /vendor/lib64/hw/camera.libcamera.so
PRODUCT_PACKAGES += \
    android.hardware.camera.provider-service.lineage \
    camera.libcamera \
    ipa_soft_simple

PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.camera=libcamera

PRODUCT_COPY_FILES += \
    $(R1_CAMERA_PATH)/camera_hal.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/camera_hal.yaml \
    $(R1_CAMERA_PATH)/configuration.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/configuration.yaml \
    frameworks/native/data/etc/android.hardware.camera.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.xml

# libcamera test application: cam -l, cam -c1 --capture=5 --file=...
PRODUCT_PACKAGES_DEBUG += \
    cam

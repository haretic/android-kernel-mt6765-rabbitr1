#
# SPDX-License-Identifier: Apache-2.0
#
# Hi-846 camera through libcamera on the mainline kernel: include from
# device.mk, and include BoardConfigCamera.mk from the device BoardConfig.mk.
#
# Capture path: hi846 -> mtk-seninf -> CAMSV1 video node (MTISP packed
# 10-bit Bayer), matched by the libcamera simple pipeline handler with the
# CPU software ISP. The raw buffers are V4L2 MMAP buffers, mapped through
# the M4U (larb3 port CAM_SOC0); the software ISP unpacks and debayers them
# with the CPU into gralloc buffers, or DMA heap buffers for the HAL's
# internal JPEG source streams.
#
# The module turns: camera_hal.yaml gives Android a back and a front camera,
# which the HAL selects by turning the motor (init.r1.camera.rc).
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
    $(R1_CAMERA_PATH)/hi846.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/simple/hi846.yaml \
    $(R1_CAMERA_PATH)/init.r1.camera.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.camera.rc \
    frameworks/native/data/etc/android.hardware.camera.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml

# libcamera test application: cam -l, cam -c1 --capture=5 --file=...
PRODUCT_PACKAGES_DEBUG += \
    cam

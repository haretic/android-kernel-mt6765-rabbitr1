# SPDX-License-Identifier: Apache-2.0
LOCAL_PATH := device/rabbit/r1

# Keep these explicit: the common bring-up defaults include a simulated battery.
TARGET_INITIAL_BRINGUP := true
TARGET_HAS_BATTERY := true
# inherit-product defers and sorts children; set the selector before common defaults.
include device/rabbit/r1/health/product.mk
# GPU: the stock RabbitOS PowerVR GE8320 userspace on the ported DDK 1.13 pvrsrvkm,
# or SwiftShader software rendering.
R1_GPU ?= pvr
ifeq ($(R1_GPU),pvr)
TARGET_GRAPHICS := pvr
TARGET_GRAPHICS_EGL := none
TARGET_GRAPHICS_VULKAN := none
TARGET_GRAPHICS_ALLOCATOR_HAL := none
else
TARGET_GRAPHICS := swiftshader
endif
TARGET_GRAPHICS_COMPOSER_HAL := drmfb-composer
# Keep the supported ARM backends without x86-only Intel cache operations.
TARGET_MINIGBM_PLATFORM := all_arm
TARGET_SUPPORTS_SUSPEND := false
TARGET_BOOT_HAL := r1
# The stock gralloc is HIDL allocator/mapper 4.0, which FCM level 7 still accepts.
ifeq ($(R1_GPU),pvr)
TARGET_FOLLOWS_LATEST_VINTF_TARGET_LEVEL := false
$(call inherit-product, vendor/rabbit/r1-pvr/r1-pvr.mk)
else
TARGET_FOLLOWS_LATEST_VINTF_TARGET_LEVEL := true
endif

TARGET_PREBUILT_KERNEL := device/rabbit/r1/prebuilt/Image.gz
TARGET_KERNEL_MIXED_MODE := false
TARGET_FORCE_PREBUILT_KERNEL := true

# Dedicated A/B extents use the upstream dm-linear driver. Stock uses virtual
# A/B; do not inherit its userspace snapshots without a working dm-user driver.
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += boot dtbo system system_ext product vendor vbmeta
PRODUCT_USE_DYNAMIC_PARTITIONS := true
PRODUCT_BUILD_SUPER_PARTITION := true
PRODUCT_VIRTUAL_AB_OTA := false
PRODUCT_VIRTUAL_AB_COMPRESSION := false

$(call inherit-product, device/mainline/common/mainline_common.mk)
$(call inherit-product, device/rabbit/r1/charging/product.mk)

PRODUCT_PACKAGES += \
    android.hardware.boot-service.r1 \
    android.hardware.boot-service.r1_recovery \
    r1-expdb

# The logger validates expdb and bounds its mapping before loading pstore.
PRODUCT_VENDOR_PROPERTIES += ro.vendor.r1.expdb.enabled=1

# One online core: ueventd lags apexd's default 1 s wait for dm-verity links,
# so boot APEX activation fails (and bpfloader with it) unless this waits longer.
PRODUCT_VENDOR_PROPERTIES += \
    apexd.config.dm_create.timeout=20000 \
    apexd.config.dm_delete.timeout=5000


# Mainline has no ashmem; libcutils only picks memfd by itself for SDK 37+ callers.
PRODUCT_SYSTEM_PROPERTIES += sys.use_memfd=true

# One slow core until secondary CPUs are brought up: AMS.systemReady alone
# outlasts the 60 s framework watchdog on first boot.
PRODUCT_VENDOR_PROPERTIES += ro.hw_timeout_multiplier=5

# Recovery-as-boot copies root/ into its ramdisk for the normal-boot switch.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/fstab.r1:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.r1 \
    $(LOCAL_PATH)/rootdir/fstab.r1:$(TARGET_COPY_OUT_ROOT)/first_stage_ramdisk/fstab.r1 \
    $(LOCAL_PATH)/rootdir/init.r1.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.rc \
    $(LOCAL_PATH)/rootdir/init.mt6765.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.mt6765.rc \
    $(LOCAL_PATH)/rootdir/r1-logcat-kmsg.sh:$(TARGET_COPY_OUT_VENDOR)/bin/r1-logcat-kmsg.sh

# Match the physical panel. Revisit density after UI testing on the device.
TARGET_SCREEN_WIDTH := 480
TARGET_SCREEN_HEIGHT := 640
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := hdpi
PRODUCT_VENDOR_PROPERTIES += ro.sf.lcd_density=140

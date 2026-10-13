# SPDX-License-Identifier: Apache-2.0
LOCAL_PATH := device/rabbit/r1

TARGET_HAS_BATTERY := true
# inherit-product defers and sorts children; set the selector before common defaults.
include device/rabbit/r1/health/product.mk
# wpa_supplicant APEX with the device overlays (connectivity/wifi).
TARGET_HOSTAPD_AND_WPA_SUPPLICANT_FORM := apex-mainline_common
$(call soong_config_set_string_list,mainline_common_apex_wpa_supplicant,include_prebuilts,\
    wpa_supplicant.conf.prebuilt wpa_supplicant_overlay.conf.r1 p2p_supplicant_overlay.conf.r1)
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

# Linaro thermal HAL; sets TARGET_THERMAL_HAL, so include before mainline_common.
include device/rabbit/r1/thermal/product.mk

$(call inherit-product, device/mainline/common/mainline_common.mk)
$(call inherit-product, device/rabbit/r1/charging/product.mk)

PRODUCT_PACKAGES += \
    android.hardware.boot-service.r1 \
    android.hardware.boot-service.r1_recovery \
    FrameworksResR1 \
    r1-expdb

# The logger validates expdb and bounds its mapping before loading pstore.
PRODUCT_VENDOR_PROPERTIES += ro.vendor.r1.expdb.enabled=1

# Factory reset protection data (stock value).
PRODUCT_VENDOR_PROPERTIES += ro.frp.pst=/dev/block/by-name/frp

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

# Handheld features; without them system_server skips the appwidget service and Launcher3 crash-loops.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/handheld_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/handheld_core_hardware.xml

# ICM-42607 accelerometer and gyroscope through the mainline IIO sensors HAL.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/init.r1.sensors.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.sensors.rc \
    $(LOCAL_PATH)/sensors/r1-sensors.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/r1-sensors.conf \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml

# Side button (PMIC power key and keypad) and the OCH1970 scroll wheel.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/keylayout/mtk-pmic-keys.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/mtk-pmic-keys.kl \
    $(LOCAL_PATH)/keylayout/mt6779-keypad.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/mt6779-keypad.kl \
    $(LOCAL_PATH)/keylayout/och1970-wheel.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/och1970-wheel.kl

# Recovery-as-boot copies root/ into its ramdisk for the normal-boot switch.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/fstab.r1:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.r1 \
    $(LOCAL_PATH)/rootdir/fstab.r1:$(TARGET_COPY_OUT_ROOT)/first_stage_ramdisk/fstab.r1 \
    $(LOCAL_PATH)/rootdir/init.r1.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.rc \
    $(LOCAL_PATH)/rootdir/ueventd.r1.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.mt6765.rc \
    $(LOCAL_PATH)/rootdir/init.mt6765.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.mt6765.rc

# Boot debugging aid; the script does nothing on non-debuggable builds.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/logging/r1-logcat-kmsg.sh:$(TARGET_COPY_OUT_SYSTEM_EXT)/bin/r1-logcat-kmsg.sh \
    $(LOCAL_PATH)/logging/init.r1-logcat-kmsg.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.r1-logcat-kmsg.rc

# Match the physical panel. Revisit density after UI testing on the device.
TARGET_SCREEN_WIDTH := 480
TARGET_SCREEN_HEIGHT := 640
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := hdpi
PRODUCT_VENDOR_PROPERTIES += ro.sf.lcd_density=140

# MT6765 AFE + MT6357 codec + AW87390 speaker amplifier.
$(call inherit-product, $(LOCAL_PATH)/audio/audio.mk)

# CONSYS Wi-Fi/BT: stock firmware, module loading, Wi-Fi powered at boot.
$(call inherit-product, $(LOCAL_PATH)/connectivity/connectivity.mk)

# MT6765 modem (MD1) on mtk-ccci.ko and the stock MediaTek RIL.
$(call inherit-product, $(LOCAL_PATH)/modem/modem.mk)

# CONSYS GPS: gps_drv.ko and the stock MediaTek mnld and GNSS HAL.
$(call inherit-product, $(LOCAL_PATH)/gnss/gnss.mk)

# Hi-846 rear camera: libcamera simple pipeline + software ISP on CAMSV.
$(call inherit-product, $(LOCAL_PATH)/camera/camera.mk)

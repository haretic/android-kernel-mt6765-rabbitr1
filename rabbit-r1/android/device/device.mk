# SPDX-License-Identifier: Apache-2.0
LOCAL_PATH := device/rabbit/r1

TARGET_HAS_BATTERY := true
# Lineage charging control in "limit" mode: the kernel charging policy stops at
# charge_stop_level and resumes at charge_start_level, keeping the system on
# USB power in between. It has no charge on/off toggle, bypass or deadline.
PRODUCT_PACKAGES += vendor.lineage.health-service.default
$(call soong_config_set_bool,lineage_health,charging_control_supports_limit,true)
$(call soong_config_set_bool,lineage_health,charging_control_supports_toggle,false)
$(call soong_config_set_bool,lineage_health,charging_control_supports_bypass,false)
$(call soong_config_set,lineage_health,charging_control_limit_start_path,/sys/devices/platform/charging-policy/charge_start_level)
$(call soong_config_set,lineage_health,charging_control_limit_stop_path,/sys/devices/platform/charging-policy/charge_stop_level)
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
# The bring-up default installs suspend_blocker; the kernel sleeps to idle
# (mem_sleep_default=s2idle in BoardConfig.mk), so let Android autosuspend.
TARGET_SUPPORTS_SUSPEND := true
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
    SettingsR1 \
    SettingsProviderR1 \
    LineageSdkR1 \
    SystemUIR1 \
    r1-expdb

# "gadget" in place of the product="tablet" strings in these apps
# (rabbit-r1/scripts/gen-gadget-overlays.py).
PRODUCT_PACKAGES += \
    GadgetCompanionDeviceManagerR1 \
    GadgetFrameworkResR1 \
    GadgetPackageInstallerR1 \
    GadgetSettingsR1 \
    GadgetSetupWizardR1 \
    GadgetStorageManagerR1 \
    GadgetSystemUIR1

PRODUCT_PACKAGES += \
    WifiResR1

# Airplane mode after the screen has been off for a set time (Settings > Battery).
PRODUCT_PACKAGES += R1RadioSleep

# Home Assistant client for the R1, preinstalled.
PRODUCT_PACKAGES += R1HA

# The logger validates expdb and bounds its mapping before loading pstore.
PRODUCT_VENDOR_PROPERTIES += ro.vendor.r1.expdb.enabled=1

# The panel goes dark below 2 of 512 (MT6370 backlight), measured on the device. The
# composer maps brightness 0, which dimming before screen-off reaches, to this level.
PRODUCT_VENDOR_PROPERTIES += ro.vendor.drmfb.backlight_min=2

# Factory reset protection data (stock value).
PRODUCT_VENDOR_PROPERTIES += ro.frp.pst=/dev/block/by-name/frp

# ueventd can lag apexd's default 1 s wait for dm-verity links while coldboot
# relabels /sys, and boot APEX activation (and bpfloader with it) then fails.
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
    $(LOCAL_PATH)/rootdir/init.recovery.r1.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.mt6765.rc \
    $(LOCAL_PATH)/rootdir/ueventd.r1.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.mt6765.rc \
    $(LOCAL_PATH)/rootdir/init.mt6765.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.mt6765.rc

# Boot debugging aid; the script does nothing on non-debuggable builds.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/logging/r1-logcat-kmsg.sh:$(TARGET_COPY_OUT_SYSTEM_EXT)/bin/r1-logcat-kmsg.sh \
    $(LOCAL_PATH)/logging/init.r1-logcat-kmsg.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.r1-logcat-kmsg.rc

# Default attestation keybox for the KeyMint HAL. The owner can override it at runtime by
# dropping a replacement at /data/vendor/keymint/keybox.xml; this is the built-in fallback.
# It is private, next to the release keys; without it KeyMint signs with its software keys.
ifneq ($(wildcard vendor/lineage-priv/keymint/keybox.xml),)
PRODUCT_COPY_FILES += \
    vendor/lineage-priv/keymint/keybox.xml:$(TARGET_COPY_OUT_VENDOR)/etc/keymint/keybox.xml
endif

# 140 matches the panel's physical density but made the UI too small to use;
# 186 is the display size picked on the device.
TARGET_SCREEN_WIDTH := 480
TARGET_SCREEN_HEIGHT := 640
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := hdpi
PRODUCT_VENDOR_PROPERTIES += ro.sf.lcd_density=186

# Build.SOC_MANUFACTURER / SOC_MODEL. The mainline arm64 kernel has no
# /proc/cpuinfo Hardware line, so apps have nothing else to identify the SoC.
PRODUCT_VENDOR_PROPERTIES += \
    ro.soc.manufacturer=Mediatek \
    ro.soc.model=MT6765

# MT6765 AFE + MT6357 codec + AW87390 speaker amplifier.
$(call inherit-product, $(LOCAL_PATH)/audio/audio.mk)

# CONSYS Wi-Fi/BT: stock firmware, module loading, Wi-Fi powered at boot.
$(call inherit-product, $(LOCAL_PATH)/connectivity/connectivity.mk)

# MT6765 modem (MD1) on mtk-ccci.ko and the stock MediaTek RIL.
$(call inherit-product, $(LOCAL_PATH)/modem/modem.mk)

# CONSYS GPS: gps_drv.ko and the stock MediaTek mnld and GNSS HAL.
$(call inherit-product, $(LOCAL_PATH)/gnss/gnss.mk)

# MT6765 VENC/VDEC: the stock MediaTek Codec2 HAL on /dev/Vcodec.
$(call inherit-product, $(LOCAL_PATH)/media/media.mk)

# Hi-846 rear camera: libcamera simple pipeline + software ISP on CAMSV.
$(call inherit-product, $(LOCAL_PATH)/camera/camera.mk)

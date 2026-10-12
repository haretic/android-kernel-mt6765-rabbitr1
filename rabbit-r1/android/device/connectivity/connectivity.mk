#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# MediaTek MT6765 CONSYS Wi-Fi and Bluetooth on the mainline kernel
# (drivers/staging/mtk-connectivity: mtk_btif_drv, wmt_drv, wmt_chrdev_wifi,
# wlan_drv_gen4m, btmtk_wmt). See README.md in this directory.
#
# This file is not matched by the optional/*/product.mk glob. Inherit it
# from the device makefile and include BoardConfigConnectivity.mk from the
# device BoardConfig.mk.
#
# The modules must sit next to TARGET_PREBUILT_KERNEL so that
# optional/prebuilt_kernel copies them to /vendor/lib/modules.
#

MTK_CONSYS_PATH := device/rabbit/r1/connectivity

# Proprietary CONSYS firmware, copied unmodified from the stock
# /vendor/firmware. WMT requests the soc1_0_* patches and WMT_SOC.cfg,
# gen4m requests WIFI_RAM_CODE_soc1_0_1_1.bin.
MTK_CONSYS_FIRMWARE_DIR ?= vendor/rabbit/r1-consys/proprietary/firmware
MTK_CONSYS_FIRMWARE := \
    WMT_SOC.cfg \
    soc1_0_patch_mcu_1_1_hdr.bin \
    soc1_0_ram_mcu_1_1_hdr.bin \
    soc1_0_ram_bt_1_1_hdr.bin \
    soc1_0_ram_wifi_1_1_hdr.bin \
    WIFI_RAM_CODE_soc1_0_1_1.bin

MTK_CONSYS_FIRMWARE_FOUND := \
    $(wildcard $(addprefix $(MTK_CONSYS_FIRMWARE_DIR)/,$(MTK_CONSYS_FIRMWARE)))
ifneq ($(words $(MTK_CONSYS_FIRMWARE_FOUND)),$(words $(MTK_CONSYS_FIRMWARE)))
$(warning MTK CONSYS firmware is incomplete in $(MTK_CONSYS_FIRMWARE_DIR); Wi-Fi and Bluetooth will not start)
endif

PRODUCT_COPY_FILES += \
    $(foreach f,$(MTK_CONSYS_FIRMWARE_FOUND),$(f):$(TARGET_COPY_OUT_VENDOR)/firmware/$(notdir $(f)))

# Module loading and /dev/wmtWifi ownership
PRODUCT_COPY_FILES += \
    $(MTK_CONSYS_PATH)/init.mtk-consys.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.mtk-consys.rc

# wpa_supplicant overlays: STA on wlan0, P2P on gen4m's p2p0.
PRODUCT_COPY_FILES += \
    $(MTK_CONSYS_PATH)/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf \
    $(MTK_CONSYS_PATH)/wifi/p2p_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant_overlay.conf

# Wi-Fi HAL: the AIDL service with the gen4m legacy HAL (wifi_hal/), which
# reports the driver's feature set, channels, country code and link stats.
PRODUCT_SOONG_NAMESPACES += \
    $(MTK_CONSYS_PATH)/wifi_hal
PRODUCT_PACKAGES += \
    android.hardware.wifi-service \
    libwifi-hal-r1 \
    libwifi-hal-r1.xml

# Wi-Fi power. "boot": power the Wi-Fi function on once at boot, wlan0 stays
# up. "hal": the Wi-Fi HAL writes 1/0 to /dev/wmtWifi when Wi-Fi is
# toggled; set the same value in BoardConfig (see BoardConfigConnectivity.mk).
MTK_CONSYS_WIFI_POWER ?= boot

ifeq ($(MTK_CONSYS_WIFI_POWER),boot)
PRODUCT_COPY_FILES += \
    $(MTK_CONSYS_PATH)/init.mtk-consys.wifi-on.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.mtk-consys.wifi-on.rc \
    $(MTK_CONSYS_PATH)/mtk-consys-wifi-on.sh:$(TARGET_COPY_OUT_VENDOR)/bin/mtk-consys-wifi-on.sh
endif

# Bluetooth: btmtk_wmt registers hci0, which the default AIDL HAL
# (TARGET_BLUETOOTH_HAL=default-aidl, on by default) opens through the HCI
# user channel. If the controller reports no address, btmtk_wmt leaves it
# unconfigured and the mainline set_bdaddr service programs one via MGMT.
PRODUCT_PACKAGES += \
    bdaddr

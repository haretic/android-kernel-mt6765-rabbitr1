#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Board side of connectivity.mk; include from the device BoardConfig.mk.
#

BOARD_VENDOR_SEPOLICY_DIRS += \
    device/rabbit/r1/connectivity/sepolicy

# Must match MTK_CONSYS_WIFI_POWER in connectivity.mk.
MTK_CONSYS_WIFI_POWER ?= boot

ifeq ($(MTK_CONSYS_WIFI_POWER),hal)
# libwifi_hal (legacy HAL fallback) powers the CONSYS Wi-Fi function
# through wmt_chrdev_wifi; wlan0 appears once the write returns.
WIFI_DRIVER_STATE_CTRL_PARAM := /dev/wmtWifi
WIFI_DRIVER_STATE_ON := 1
WIFI_DRIVER_STATE_OFF := 0
endif

# SPDX-License-Identifier: Apache-2.0
# Linaro's generic AIDL thermal HAL (hardware/linaro/libpm), selected through
# device/mainline/common/optional/thermal-hal_linaro-libpm. Include, don't
# inherit, so the selector is set before mainline_common.mk reads it.
TARGET_THERMAL_HAL := linaro-libpm

# The HAL reads /vendor/etc/$(vendor.thermal.config), thermal.json by default.
PRODUCT_COPY_FILES += \
    device/rabbit/r1/thermal/thermal.json:$(TARGET_COPY_OUT_VENDOR)/etc/thermal.json

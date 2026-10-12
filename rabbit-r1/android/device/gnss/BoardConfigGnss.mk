# SPDX-License-Identifier: Apache-2.0
# Board side of gnss.mk; include from BoardConfig.mk together with it.

R1_GNSS_PATH := device/rabbit/r1/gnss

# The stock GNSS blobs are installed verbatim (PRODUCT_COPY_FILES of ELFs).
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

BOARD_VENDOR_SEPOLICY_DIRS += $(R1_GNSS_PATH)/sepolicy

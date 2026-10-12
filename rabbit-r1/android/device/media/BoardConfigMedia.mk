# SPDX-License-Identifier: Apache-2.0
# Board side of media.mk; include from BoardConfig.mk together with it.

R1_MEDIA_PATH := device/rabbit/r1/media

# The stock codec blobs are installed verbatim (PRODUCT_COPY_FILES of ELFs).
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

BOARD_VENDOR_SEPOLICY_DIRS += $(R1_MEDIA_PATH)/sepolicy

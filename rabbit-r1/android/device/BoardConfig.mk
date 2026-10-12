# SPDX-License-Identifier: Apache-2.0
DEVICE_PATH := device/rabbit/r1
MAINLINE_COMMON_PATH := device/mainline/common

TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_VARIANT := cortex-a53
TARGET_CPU_ABI := arm64-v8a

# 32-bit native code for stock vendor daemons that only exist as arm32
# (gsm0710muxd). Apps and zygote stay 64-bit (core_64_bit_only.mk).
TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_VARIANT := cortex-a53
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
# With a 2nd arch mediaserver and drmserver prefer 32-bit, and there's no
# 32-bit gralloc mapper to map buffers with. Build both and start the one
# matching the 64-bit zygote.
TARGET_DYNAMIC_64_32_MEDIASERVER := true
TARGET_DYNAMIC_64_32_DRMSERVER := true
TARGET_BOARD_PLATFORM := mt6765
TARGET_NO_BOOTLOADER := true

TARGET_PREBUILT_KERNEL := $(DEVICE_PATH)/prebuilt/Image.gz
TARGET_KERNEL_MIXED_MODE := false
include $(MAINLINE_COMMON_PATH)/BoardConfigMainlineCommon.mk
include $(DEVICE_PATH)/charging/board.mk
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/system_ext
include $(DEVICE_PATH)/connectivity/BoardConfigConnectivity.mk
include $(DEVICE_PATH)/camera/BoardConfigCamera.mk
ifeq ($(TARGET_PRODUCT),lineage_r1_soc)
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/health/sepolicy
endif
# expdb requires its bounded mapper before pstore_blk is loaded.
BOARD_VENDOR_KERNEL_MODULES_LOAD := false

# LK expects the stock v2 header and an Android DT table in its DTB section.
# table.dtb is a generated table, despite the extension required by Android make.
BOARD_BOOT_HEADER_VERSION := 2
BOARD_KERNEL_BASE := 0
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_IMAGE_NAME := Image.gz
BOARD_INCLUDE_DTB_IN_BOOTIMG := true
BOARD_PREBUILT_DTBIMAGE_DIR := $(DEVICE_PATH)/prebuilt/dtb
BOARD_PREBUILT_DTBOIMAGE := $(DEVICE_PATH)/prebuilt/dtbo.img
BOARD_MKBOOTIMG_ARGS += \
    --header_version $(BOARD_BOOT_HEADER_VERSION) \
    --kernel_offset 0x40080000 \
    --ramdisk_offset 0x51b00000 \
    --tags_offset 0x47880000 \
    --dtb_offset 0x47880000

BOARD_KERNEL_CMDLINE += $(filter-out binder.impl=rust,$(MAINLINE_COMMON_KERNEL_PARAMS))
BOARD_KERNEL_CMDLINE += $(MAINLINE_COMMON_ANDROIDBOOT_PARAMS)
BOARD_KERNEL_CMDLINE += bootopt=64S3,32N2,64N2 androidboot.fstab_suffix=r1
BOARD_KERNEL_CMDLINE += earlycon=uart8250,mmio32,0x11002000 console=ttyS0,921600n8
BOARD_KERNEL_CMDLINE += maxcpus=1 clk_ignore_unused regulator_ignore_unused
# Reboot on panic; the kernel default would hang.
BOARD_KERNEL_CMDLINE += panic=-1

BOARD_USES_RECOVERY_AS_BOOT := true
TARGET_NO_RECOVERY := false
BOARD_BOOTIMAGE_PARTITION_SIZE := 33554432
BOARD_DTBOIMG_PARTITION_SIZE := 8388608
BOARD_RECOVERYIMAGE_PARTITION_SIZE := $(BOARD_BOOTIMAGE_PARTITION_SIZE)
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/rootdir/fstab.r1
TARGET_RECOVERY_PIXEL_FORMAT := BGRA_8888

TARGET_USERIMAGES_USE_EXT4 := true
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_PARTITION_SIZE := 51539607552
TARGET_COPY_OUT_SYSTEM_EXT := system_ext
TARGET_COPY_OUT_PRODUCT := product
TARGET_COPY_OUT_VENDOR := vendor
BOARD_USES_METADATA_PARTITION := true

# Stock scatter and all three liblp metadata copies agree on the super size.
# Reserve 4 GiB per slot; unlike stock virtual A/B, both slots need real extents.
BOARD_SUPER_PARTITION_SIZE := 8792064000
BOARD_SUPER_PARTITION_GROUPS := r1_dynamic_partitions
BOARD_R1_DYNAMIC_PARTITIONS_SIZE := 4294967296
BOARD_R1_DYNAMIC_PARTITIONS_PARTITION_LIST := system system_ext product vendor
BOARD_BUILD_SUPER_IMAGE_BY_DEFAULT := true

# Development images use the Android build's test keys. LK must remain unlocked.
BOARD_AVB_ENABLE := true
BOARD_AVB_MAKE_VBMETA_IMAGE_ARGS += --flags 3

TARGET_ENABLE_MEDIADRM_64 := true
TARGET_LMKD_STATS_LOG := true

ifeq ($(R1_GPU),pvr)
include vendor/rabbit/r1-pvr/BoardConfigPvr.mk
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/pvr
endif

# Modem: sepolicy and framework matrix for modem/modem.mk.
include $(DEVICE_PATH)/modem/BoardConfigModem.mk
# r1-md-probe.ko is a bring-up diagnostic that binds the same DT node as
# mtk-ccci.ko; the prebuilt_kernel glob must not install it into vendor.
BOARD_VENDOR_KERNEL_MODULES := $(filter-out %/r1-md-probe.ko,$(BOARD_VENDOR_KERNEL_MODULES))

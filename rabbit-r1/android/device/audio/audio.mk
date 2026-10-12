# SPDX-License-Identifier: Apache-2.0
# Speaker/mic audio on the mainline kernel: include from device.mk.
#
# HAL: device/mainline/common's default TARGET_AUDIO_HAL=mainline (AIDL HAL
# APEX on alsa-lib + UCM); no audio_policy_configuration.xml is needed.
#
# Kernel: snd-soc-mt6765-afe, mt6765-mt6357 and the MT6357 codec are built
# in. The AW87390 speaker amplifier driver is a module so that its firmware
# loads from /vendor/firmware; the card appears once it is inserted.
#
# Generated inputs (not committed; see aw87xxx-acf-to-aw87390.py):
#   prebuilt/firmware/aw87390_acf.bin    from stock vendor/firmware/aw87xxx_acf.bin
# The amplifier modules reach /vendor/lib/modules with the other kernel modules.

R1_AUDIO_PATH := device/rabbit/r1/audio

# Only the r1 card profile, on top of the base UCM files the HAL APEX requires.
TARGET_AUDIO_MAINLINE_UCM_PROFILES := none

PRODUCT_COPY_FILES += \
    $(R1_AUDIO_PATH)/ucm2/conf.d/mt6765-mt6357/rabbit-r1.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/conf.d/mt6765-mt6357/rabbit-r1.conf \
    $(R1_AUDIO_PATH)/ucm2/Rabbit/r1/HiFi.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/Rabbit/r1/HiFi.conf \
    $(R1_AUDIO_PATH)/init.r1.audio.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.audio.rc \
    $(R1_AUDIO_PATH)/init.r1.audio-restart.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.r1.audio-restart.rc \
    device/rabbit/r1/prebuilt/firmware/aw87390_acf.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/aw87390_acf.bin

# Use the r1 card only, and give the amplifier module time to bind it.
PRODUCT_VENDOR_PROPERTIES += \
    vendor.audio.mainline.cards=rabbit-r1 \
    vendor.audio.mainline.wait_for_cards_ms=10000

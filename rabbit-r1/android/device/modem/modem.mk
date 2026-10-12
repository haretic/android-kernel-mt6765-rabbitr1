# SPDX-License-Identifier: Apache-2.0
# MT6765 modem (MD1) and the stock MediaTek RIL on the mainline kernel.
# DRAFT for Stage 3: include from device.mk once the kernel side (mtk-ccci.ko,
# DT &modem) has brought the modem to READY by hand. See README.md.
#
# Blobs: vendor/rabbit/r1-modem/proprietary/ holds the files in blobs.txt,
# copied verbatim from out/stock-vendor/tree except libnvram.so and
# libsysenv.so, which get "patchelf --add-needed libr1_base_shim.so", and
# libmtkutils.so and its users, renamed by rename-mtkutils.py (README.md).
# Board side: BoardConfigModem.mk (sepolicy, framework matrix).

R1_MODEM_PATH := device/rabbit/r1/modem
R1_MODEM_BLOBS := vendor/rabbit/r1-modem/proprietary

# Generated from blobs.txt; keep the two in sync.
PRODUCT_COPY_FILES += \
    $(R1_MODEM_BLOBS)/bin/ccci_mdinit:$(TARGET_COPY_OUT_VENDOR)/bin/ccci_mdinit \
    $(R1_MODEM_BLOBS)/bin/ccci_rpcd:$(TARGET_COPY_OUT_VENDOR)/bin/ccci_rpcd \
    $(R1_MODEM_BLOBS)/bin/gsm0710muxd:$(TARGET_COPY_OUT_VENDOR)/bin/gsm0710muxd \
    $(R1_MODEM_BLOBS)/lib/libmtkcutils.so:$(TARGET_COPY_OUT_VENDOR)/lib/libmtkcutils.so \
    $(R1_MODEM_BLOBS)/lib/libmtkproperty.so:$(TARGET_COPY_OUT_VENDOR)/lib/libmtkproperty.so \
    $(R1_MODEM_BLOBS)/lib/libmtkrillog.so:$(TARGET_COPY_OUT_VENDOR)/lib/libmtkrillog.so \
    $(R1_MODEM_BLOBS)/bin/hw/mtkfusionrild:$(TARGET_COPY_OUT_VENDOR)/bin/hw/mtkfusionrild \
    $(R1_MODEM_BLOBS)/bin/hw/vendor.mediatek.hardware.nvram@1.1-service:$(TARGET_COPY_OUT_VENDOR)/bin/hw/vendor.mediatek.hardware.nvram@1.1-service \
    $(R1_MODEM_BLOBS)/bin/netdagent:$(TARGET_COPY_OUT_VENDOR)/bin/netdagent \
    $(R1_MODEM_BLOBS)/bin/nvram_daemon:$(TARGET_COPY_OUT_VENDOR)/bin/nvram_daemon \
    $(R1_MODEM_BLOBS)/etc/ecc_list.xml:$(TARGET_COPY_OUT_VENDOR)/etc/ecc_list.xml \
    $(R1_MODEM_BLOBS)/etc/ecc_list_preference.xml:$(TARGET_COPY_OUT_VENDOR)/etc/ecc_list_preference.xml \
    $(R1_MODEM_BLOBS)/lib64/hw/vendor.mediatek.hardware.nvram@1.1-impl.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/vendor.mediatek.hardware.nvram@1.1-impl.so \
    $(R1_MODEM_BLOBS)/lib64/libaedv.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libaedv.so \
    $(R1_MODEM_BLOBS)/lib64/libcapctrl.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libcapctrl.so \
    $(R1_MODEM_BLOBS)/lib64/libcares_naptr.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libcares_naptr.so \
    $(R1_MODEM_BLOBS)/lib64/libcarrierconfig.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libcarrierconfig.so \
    $(R1_MODEM_BLOBS)/lib64/libccci_util.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libccci_util.so \
    $(R1_MODEM_BLOBS)/lib64/libconnfem.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libconnfem.so \
    $(R1_MODEM_BLOBS)/lib64/libcustom_nvram.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libcustom_nvram.so \
    $(R1_MODEM_BLOBS)/lib64/libfile_op.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libfile_op.so \
    $(R1_MODEM_BLOBS)/lib64/libforkexecwrap.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libforkexecwrap.so \
    $(R1_MODEM_BLOBS)/lib64/libhwm.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libhwm.so \
    $(R1_MODEM_BLOBS)/lib64/libifcutils_mtk.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libifcutils_mtk.so \
    $(R1_MODEM_BLOBS)/lib64/libmipc.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmipc.so \
    $(R1_MODEM_BLOBS)/lib64/libmnetlink_v104.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmnetlink_v104.so \
    $(R1_MODEM_BLOBS)/lib64/libmtk-ril.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtk-ril.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkares.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkares.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkconfig.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkconfig.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkconfigutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkconfigutils.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkcutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkcutils.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkhardware_legacy.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkhardware_legacy.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkmipc-ril.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkmipc-ril.so \
    $(R1_MODEM_BLOBS)/lib64/libmtknetcap.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtknetcap.so \
    $(R1_MODEM_BLOBS)/lib64/libmtknetutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtknetutils.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkproperty.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkproperty.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkrillog.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkrillog.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkrilutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkrilutils.so \
    $(R1_MODEM_BLOBS)/lib64/libmtksysutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtksysutils.so \
    $(R1_MODEM_BLOBS)/lib64/libmtktinyxml.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtktinyxml.so \
    $(R1_MODEM_BLOBS)/lib64/libmtkutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmtkutils.so \
    $(R1_MODEM_BLOBS)/lib64/libnvram.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libnvram.so \
    $(R1_MODEM_BLOBS)/lib64/libnvram_sec.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libnvram_sec.so \
    $(R1_MODEM_BLOBS)/lib64/libratconfig.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libratconfig.so \
    $(R1_MODEM_BLOBS)/lib64/librilfusion.so:$(TARGET_COPY_OUT_VENDOR)/lib64/librilfusion.so \
    $(R1_MODEM_BLOBS)/lib64/libstorage_otp.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libstorage_otp.so \
    $(R1_MODEM_BLOBS)/lib64/libsysenv.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libsysenv.so \
    $(R1_MODEM_BLOBS)/lib64/libtrm.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libtrm.so \
    $(R1_MODEM_BLOBS)/lib64/libvia-ril.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libvia-ril.so \
    $(R1_MODEM_BLOBS)/lib64/libviamipc-ril.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libviamipc-ril.so \
    $(R1_MODEM_BLOBS)/lib64/libwpfa.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libwpfa.so \
    $(R1_MODEM_BLOBS)/lib64/vendor.mediatek.hardware.mtkradioex@3.0.so:$(TARGET_COPY_OUT_VENDOR)/lib64/vendor.mediatek.hardware.mtkradioex@3.0.so \
    $(R1_MODEM_BLOBS)/lib64/vendor.mediatek.hardware.netdagent@1.0.so:$(TARGET_COPY_OUT_VENDOR)/lib64/vendor.mediatek.hardware.netdagent@1.0.so \
    $(R1_MODEM_BLOBS)/lib64/vendor.mediatek.hardware.nvram@1.0.so:$(TARGET_COPY_OUT_VENDOR)/lib64/vendor.mediatek.hardware.nvram@1.0.so \
    $(R1_MODEM_BLOBS)/lib64/vendor.mediatek.hardware.nvram@1.1.so:$(TARGET_COPY_OUT_VENDOR)/lib64/vendor.mediatek.hardware.nvram@1.1.so

# Old libbase Trim/Basename for libnvram.so and libsysenv.so.
PRODUCT_PACKAGES += libr1_base_shim

# gsm0710muxd is arm32 only (lib/ above); its AOSP dependencies.
PRODUCT_PACKAGES += \
    libc++.vendor:32 \
    liblog:32

# AOSP HIDL interfaces the blobs link against, as vendor variants. The stock
# copies are not shipped so that they match our libhidlbase.
PRODUCT_PACKAGES += \
    android.hardware.radio@1.0.vendor \
    android.hardware.radio@1.1.vendor \
    android.hardware.radio@1.2.vendor \
    android.hardware.radio@1.3.vendor \
    android.hardware.radio@1.4.vendor \
    android.hardware.radio@1.5.vendor \
    android.hardware.radio@1.6.vendor \
    android.hardware.radio.config@1.0.vendor \
    android.hardware.radio.config@1.1.vendor \
    android.hardware.radio.config@1.2.vendor \
    android.hardware.radio.config@1.3.vendor \
    android.hidl.safe_union@1.0.vendor \
    libhidltransport.vendor \
    libhardware_legacy.vendor \
    libunwindstack.vendor \
    hwservicemanager

# Kernel module, init, ueventd, NV mount gate.
PRODUCT_COPY_FILES += \
    $(R1_MODEM_PATH)/init.r1.modem.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.r1.modem.rc \
    $(R1_MODEM_PATH)/ueventd.r1-modem.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.r1-modem.rc \
    $(R1_MODEM_PATH)/r1-modem-nvcheck.sh:$(TARGET_COPY_OUT_VENDOR)/bin/r1-modem-nvcheck.sh

DEVICE_MANIFEST_FILE += $(R1_MODEM_PATH)/manifest-modem.xml

# Telephony features. gsm.xml also declares calling; voice has no audio path
# until Stage 5 (modem PCM DAI + speech control), data and SMS work without it.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml

# Single SIM, as stock. Values are the stock vendor build.prop ones.
PRODUCT_VENDOR_PROPERTIES += \
    persist.radio.multisim.config=ss \
    ro.vendor.radio.max.multisim=ss \
    persist.vendor.radio.msimmode=ss \
    telephony.active_modems.max_count=1 \
    ro.telephony.default_network=10,10,10,10 \
    ro.vendor.mtk_ril_mode=c6m_1rild \
    ro.vendor.md_prop_ver=1 \
    ro.vendor.num_md_protocol=2 \
    ro.vendor.mtk_single_bin_modem_support=1 \
    ro.vendor.mtk_md_world_mode_support=1 \
    ro.vendor.mtk_eccci_c2k=1 \
    ro.vendor.mtk_c2k_support=1 \
    ro.vendor.mtk_c2k_lte_mode=2 \
    ro.vendor.mtk_lte_support=1 \
    ro.vendor.mtk_protocol1_rat_config=C/Lf/Lt/W/T/G \
    ro.vendor.mtk_ps1_rat=C/Lf/Lt/W/T/G \
    persist.vendor.radio.mtk_ps2_rat=L/W/G \
    persist.vendor.radio.mtk_ps3_rat=G \
    ro.vendor.mtk_rild_read_imsi=1 \
    ro.vendor.sim_me_lock_mode=0 \
    ro.vendor.mtk_sim_card_onoff=3 \
    ro.vendor.mtk_external_sim_only_slots=0 \
    ro.vendor.mtk_world_phone_policy=0 \
    persist.vendor.mtk_sim_switch_policy=2 \
    ro.vendor.mtk_data_config=1 \
    ro.vendor.mtk_fd_support=1 \
    persist.vendor.radio.fd.counter=150 \
    persist.vendor.radio.fd.off.counter=50 \
    persist.vendor.radio.fd.r8.counter=150 \
    persist.vendor.radio.fd.off.r8.counter=50 \
    persist.vendor.radio.flashless.fsm=0 \
    persist.vendor.radio.flashless.fsm_cst=0 \
    persist.vendor.radio.flashless.fsm_rw=0 \
    persist.vendor.md_c2k_cap_dep_check=0 \
    persist.vendor.vilte_support=0 \
    ro.vendor.md_auto_setup_ims=0 \
    persist.vendor.mims_support=1 \
    ro.vendor.md_apps.support=1 \
    ro.vendor.md_apps.load_type=user \
    ro.vendor.md_apps.load_gencfg=GEN93_USER
# ro.vendor.md_auto_setup_ims is 1 on stock. 0 keeps the modem from bringing up
# an IMS PDN that nothing on our side (no MTK ImsService) would ever use.

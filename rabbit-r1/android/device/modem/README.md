# Modem (Stage 3 draft)

Android side of cellular on the Rabbit r1: the stock MediaTek modem userspace
(Android 12 blobs) running on the mainline kernel's `mtk-ccci.ko`. These are
drafts. Nothing here is included by `device.mk` or `BoardConfig.mk` yet. Wire
them in only after the kernel stage passes by hand: `ccci_mdinit` brings MD1 to
READY, and on `/dev/ttyC0` `AT+CPIN?` answers READY.

Plan and background: `docs/research/r1-modem-ril-feasibility.md`.

## Files

| File | What it is |
|---|---|
| `modem.mk` | Product side: blobs, shim, AOSP HIDL `.vendor` libraries, rc and ueventd files, VINTF fragment, telephony feature, stock radio properties. |
| `BoardConfigModem.mk` | Board side: sepolicy dir, framework matrix fragment, `BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES`. |
| `blobs.txt` | The 45 files to import from `out/stock-vendor/tree`. |
| `shim/` | `libr1_base_shim.so`: the old libbase `Trim(const std::string&)` and `Basename(const std::string&)`. |
| `init.r1.modem.rc` | `insmod mtk-ccci.ko`; the stock services, all gated on the NV check. |
| `r1-modem-nvcheck.sh` | Sets `vendor.r1.modem.nv_ready=1` only if all four NV filesystems are mounted rw. |
| `ueventd.r1-modem.rc` | Owners and modes for `/dev/ccci*`, `/dev/ttyC*`, `/sys/kernel/ccci`, nvram and proinfo. |
| `fstab.modem` | Lines to append to `rootdir/fstab.r1`. This file is not installed on its own. |
| `manifest-modem.xml` | HIDL IRadio 1.6, ISap 1.2, IRadioConfig 1.3, and the MediaTek mtkradioex, nvram and netdagent instances. |
| `framework_compatibility_matrix-modem.xml` | Matrix entries for the instances that no AOSP framework compatibility matrix (FCM) lists. |
| `sepolicy/` | Types, labels and transitions. The domains are permissive. |

## Wiring (edits to existing files, left to you)

1. **`device.mk`**: `$(call inherit-product, $(LOCAL_PATH)/modem/modem.mk)`.
2. **`BoardConfig.mk`**: `include $(DEVICE_PATH)/modem/BoardConfigModem.mk`.
3. **`rootdir/fstab.r1`**: append the lines in `fstab.modem`.
4. **`rootdir/ueventd.r1.rc`**: add `import /vendor/etc/ueventd.r1-modem.rc`. ueventd only reads `/vendor/etc/ueventd.rc` and what that imports. Mainline common imports `ueventd.${ro.board.platform}.rc`, which is our `ueventd.mt6765.rc`.
5. **`lineage_r1.mk`**: stop being wifi-only.
   - Replace `vendor/lineage/config/common_full_tablet_wifionly.mk` with `common_full_tablet.mk`, which pulls in `telephony.mk` (APNs, messaging, Stk).
   - Add these after `full_base.mk`:
     - `$(SRC_TARGET_DIR)/product/telephony_system.mk`
     - `telephony_system_ext.mk`
     - `telephony_product.mk`
   - Do **not** use `full_base_telephony.mk` or `telephony_vendor.mk`. They add AOSP `rild`, which would compete with mtkfusionrild for `IRadio/slot1`.
   - `PRODUCT_CHARACTERISTICS := tablet` can stay. It only selects resources.
6. **Kernel**: `CONFIG_MTK_CCCI=m` is set in `rabbit_r1_defconfig` on the `rabbit-r1/modem` branch. Put `mtk-ccci.ko` in `prebuilt/` with the other modules. Do **not** ship `r1-md-probe.ko` in the image, because it binds the same DT node.
7. **Blobs**: create `vendor/rabbit/r1-modem/proprietary/`, the same way as `vendor/rabbit/r1-pvr`.
   - Copy every path in `blobs.txt` from `out/stock-vendor/tree`.
   - Then run `patchelf --add-needed libr1_base_shim.so` on `lib64/libnvram.so` and `lib64/libsysenv.so`. Those two are the only unresolved imports against our Android 17 libraries; this was checked with `nm -D` over the whole closure.

## Boot flow

- **post-fs:** `insmod mtk-ccci.ko`.
  - The driver binds the `mediatek,mt6765-ccci` node and creates the ports, `/dev/ccci_*` and `/sys/kernel/ccci`.
  - It does not boot the modem. Nothing on the kernel side loads it automatically, because there is no `MODULE_DEVICE_TABLE`.
- **fs, before post-fs:** `mount_all /vendor/etc/fstab.r1 --early` mounts `protect_f`, `protect_s`, `nvdata` and `nvcfg`.
- **post-fs-data:** the stock directories are created, then `r1-modem-nvcheck.sh` runs.
- **`vendor.r1.modem.nv_ready=1`:** `nvram_daemon`, the nvram HAL, `ccci_rpcd` and `ccci_mdinit` start.
  - `ccci_mdinit` waits for `vendor.service.nvram_init` from `nvram_daemon`.
  - It then sets the boot data through ioctls and starts MD1.
  - It serves the modem's NV files over `ccci_fs`.
  - After the modem reaches READY it `ctl.start`s `vendor.ril-daemon-mtk`.
- **mtkfusionrild:** registers `IRadio/slot1`, and AOSP telephony binds it as HIDL 1.6.
- **Data:** data runs on `ccmni0..`, which mtkfusionrild and netdagent configure.

## NV data rules

- Never `formattable`. The NV filesystems hold this unit's IMEI and RF calibration. Stock marks them formattable; we never do.
- They are mounted read-write. The modem and `nvram_daemon` write them during normal operation.
- Take a fresh backup of `nvdata protect1 protect2 nvram proinfo` (`nvcfg` too) before the first boot with this enabled. The previous backup is `out/device-backup-review/modem-nv-backup-20261012/`.
- If any NV filesystem fails to mount, nothing that writes NV data starts. `nofail` keeps the boot going, and the nvcheck script keeps the modem off. Without this gate, `nvram_daemon` would rebuild NV on the `/mnt` tmpfs and could back that up over the `nvram` partition.
- Logs: LK passes the IMEI on the kernel command line (`androidboot.lk_imei`), and the modem and RIL print identifiers too. Scrub kmsg, logcat and radio logs before sharing them.

## SELinux

- `sepolicy/` declares every domain, exec type, device, file and property type, plus the hwservice labels. All modem domains are `permissive`, including the platform `rild` domain that mtkfusionrild runs in.
- That works on userdebug, but a user build fails its permissive-domain check by design.
- File and device type names are the stock ones. The NV ext4 filesystems already carry those labels, so nothing needs relabelling.
- Stage 4 replaces the `permissive` lines with rules ported from the stock `etc/selinux/vendor_sepolicy.cil`. That file has 221 rules for `ccci_mdinit`, `ccci_rpcd` and `nvram_daemon` alone.

## VINTF

- Our device manifest is `target-level="7"`. Android 17's `compatibility_matrix.7.xml` still accepts HIDL `IRadio` 1.5-1.6 (`slot1..3`), `ISap` 1.2 and `IRadioConfig` 1.1/1.3, and `RIL.java` still binds HIDL 1.6.
- Do not raise the target level while using this RIL. The fallback is `android.hardware.radio-service.compat`, which turns a HIDL IRadio into AIDL.
- mtkfusionrild also registers `IRadio/em1`, `se1`, `imsAospSlot1` and the `IMtkRadioEx` instances. They are declared, as on stock, because hwservicemanager refuses undeclared instances. They are listed as optional in the framework matrix fragment.

## Not covered here

- **Voice audio (Stage 5).** It needs a modem PCM DAI and the speech-control protocol in our audio HAL. `android.hardware.telephony.gsm.xml` still declares calling, so calls connect without audio until then. If that is unwanted, use the individual `radio.access`, `subscription`, `data` and `messaging` feature files instead of `gsm.xml`.
- **IMS/VoLTE.** There is no MediaTek ImsService. `ro.vendor.md_auto_setup_ims=0` keeps the modem from opening an IMS PDN that nothing would use. Stock sets it to 1.
- **The stock services not carried over:**
  - `gsm0710muxd` and `muxreport`: disabled on stock as well.
  - `ccci3_mdinit`: there is no MD3.
  - `volte_*`, `atcid` and `md_apps`.
- **Flight mode and power.** `clk_buf` flight mode, PBM and the VCORE hold are no-ops in the kernel port. `TARGET_SUPPORTS_SUSPEND` is false, so suspend interplay is untested.

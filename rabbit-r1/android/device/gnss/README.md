# GNSS (GPS)

Android side of GPS on the Rabbit r1: the stock MediaTek GNSS userspace
(Android 12 blobs) running on the mainline kernel's `gps_drv.ko`
(`drivers/staging/mtk-connectivity/gps`, kernel branch `rabbit-r1/gnss`).

The GPS receiver is a function of the MT6765 CONSYS, like Wi-Fi and
Bluetooth. Opening `/dev/stpgps` makes WMT power the GPS function on
(VCN28, the external LNA enable on GPIO177, the WMT GPS on command).
`mnld` then talks to the receiver firmware over the STP GPS channel through
that node. `android.hardware.gnss-service.mediatek` is a thin HAL. It relays
the framework's requests to `mnld` over abstract datagram sockets
(`mtk_hal2mnl`, `mtk_mnl2hal`).

## Files

| File | What it is |
|---|---|
| `gnss.mk` | Product side: blobs, the VNDK 31 AIDL stub, AOSP `.vendor` libraries, rc and ueventd files, VINTF fragment, feature XML, properties. |
| `BoardConfigGnss.mk` | Board side: sepolicy dir, `BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES`. |
| `blobs.txt` | The 9 files to import from `out/stock-vendor/tree`. |
| `init.r1.gnss.rc` | `gps_drv` through vendor_modprobe on `vendor.connsys.driver.ready=yes`, the `/data/vendor/gps` directories, and the stock `mnld` and `vendor.gnss-default` services. |
| `ueventd.r1-gnss.rc` | `/dev/stpgps` as gps:gps 0660. |
| `manifest-gnss.xml` | AIDL `IGnss/default` V1 and HIDL `@2.1::IGnss/default` (+ `@1.1`). |
| `sepolicy/` | The `mnld` domain, device, data, socket and property types, and the `hal_gnss_default` additions. |

## Wiring (edits to existing files, left to you)

1. **`device.mk`**: `$(call inherit-product, $(LOCAL_PATH)/gnss/gnss.mk)`. Put it after `modem/modem.mk` and `connectivity/connectivity.mk`. Both must stay included:
   - modem.mk provides `libnvram.so` (with the base shim), `libhidltransport.vendor` and `hwservicemanager`, which mnld and the HAL use.
   - The GNSS sepolicy reads the modem's `vendor_mtk_nvram_prop`, `vendor_mtk_ril_prop` and `vendor_mtk_radio_config_prop`.
   - connectivity.mk loads `wmt_drv`.
2. **`BoardConfig.mk`**: `include $(DEVICE_PATH)/gnss/BoardConfigGnss.mk`.
3. **`rootdir/ueventd.r1.rc`**: add `import /vendor/etc/ueventd.r1-gnss.rc`.
4. **`connectivity/init.mtk-consys.rc`**: already done in this branch. It sets `vendor.connsys.driver.ready yes` after the CONSYS modules, as stock `wmt_loader` does. That triggers the `gps_drv.ko` insmod, and mnld reads the property.
5. **Kernel**: `CONFIG_MTK_CONN_GPS=m` (rabbit_r1_defconfig on `rabbit-r1/gnss`). Put `gps_drv.ko` in `prebuilt/` with the other modules. Also take the new DTB: it adds the GPS LNA pin states to the CONSYS node. Without them, WMT cannot switch the external LNA on.
6. **Blobs**: create `vendor/rabbit/r1-gnss/proprietary/`, the same way as `vendor/rabbit/r1-modem`.
   - Copy every path in `blobs.txt` from `out/stock-vendor/tree`, keeping the relative path.
   - One patch: the stock `libmnl.so` and `libsensorndkbridge.so` get a `libmtk` prefix (soname, and mnld's NEEDED), because AOSP modules own both names in `/vendor/lib64`. The closure was checked with `nm -D` against the Android 17 build: nothing is unresolved. The one exception is `property_get` in `libmnl.so`, which comes from mnld's `libcutils`.
   - `android.hardware.gnss-V1-ndk_platform.so` is not a blob. gnss.mk installs it straight from `prebuilts/vndk/v31`.

## Why the pieces are there

- **`android.hardware.gnss-V1-ndk_platform.so`**: the AIDL IGnss V1 NDK stubs, under the Android 12 name the HAL links. Android 12 shipped it in the VNDK APEX, and Android 13 renamed it to `-ndk`. The VNDK 31 snapshot copy is the exact library the blobs were built against.
- **AOSP `.vendor` libraries** (HIDL gnss 1.0-2.1, measurement_corrections, visibility_control, sensors@1.0, sensorservice@1.0, libcurl, libssl): VNDK on Android 12. They are now built for vendor, so they match our libhidlbase and BoringSSL. The stock copies are not shipped.
- **`libmtksensorndkbridge.so`** is the stock `libsensorndkbridge.so`. mnld needs its HIDL flavour (`convertToSensorEvent` and the `@1.0::ISensorManager` client). The AOSP module of that name is AIDL-based and owns the original path, hence the rename.
- **`persist.vendor.connsys.chipid=0x6765`**: stock `wmt_loader` sets this, and mnld selects its chip configuration from it. We do not run wmt_loader (`CONFIG_MTK_CONN_WMT_AUTOLAUNCH`).
- **VINTF**: the service registers AIDL IGnss V1 and the passthrough HIDL IGnss 2.1, as on stock.
  - With AIDL IGnss older than V2, the framework (`services/core/jni/gnss/Gnss.cpp`) takes the location path from HIDL 2.1. It uses AIDL V1 for the extensions it covers.
  - hwservicemanager refuses a HIDL instance unless its parents are declared too, hence `@1.1::IGnss`. AOSP's own 2.1 sample manifest does the same.
  - FCM 7 (our target level) lists HIDL IGnss 2.0-1 and AIDL IGnss 1-2.
- **`/dev/gps_emi`**: the stock MT6765 `gps_drv.ko` creates it, and so does ours, but on stock mnld cannot open it. There is no ueventd entry, and the `chmod` in the stock rc runs before the module is loaded. Its EMI init ioctl also always fails on MT6765. We keep it root 0600 so that mnld takes the same path as on stock.

## SELinux

- `mnld` gets a vendor domain. The rules are the relevant subset of the stock `vendor_sepolicy.cil`:
  - `/dev/stpgps`;
  - `/data/vendor/gps` and `/data/vendor/log/gps`;
  - the HAL datagram sockets;
  - NV data for the GPS record and the almanac;
  - the HIDL sensor service for dead reckoning;
  - its properties;
  - `net_domain` for EPO downloads.
- The HAL binary is labelled `hal_gnss_default_exec`, so it runs in the AOSP `hal_gnss_default` domain. That domain already registers the AIDL and HIDL IGnss. Only the socket rules and `wake_alarm` are added.
- The domains are enforcing, unlike the modem's. If GPS misbehaves, look for denials first (`adb shell dmesg | grep avc`, `logcat -b all | grep avc`). On userdebug, `adb shell setenforce 0` rules policy out quickly.

## Not covered

- **AGPS/SUPL**: `mtk_agpsd` (`etc/init/mtk_agpsd_p.rc`, `etc/gnss/agps_profiles_conf2.xml` and the carrier files) is not shipped. A standalone fix does not need it. Expect mnld to log failed sends to its `mtk_mnl2agps` socket. Assistance then comes only from mnld's own EPO/QEPO download (libcurl, over any data connection) and the time and location the framework injects. Shipping it means adding `bin/mtk_agpsd` (it needs `libexpat.vendor` and LLNDK `libandroid_net`), its rc, the `agpsd` sockets and a domain.
- **Connsys firmware logs** (`/dev/fw_log_gps`): they need the dedicated connsys log path, which `wmt_drv` is built without.
- **GPS desense**: on stock, while GPS ran, `helio-dvfsrc` kept DRAM off the OPP whose clock desenses L1. This kernel has no DVFSRC driver, so DRAM stays wherever the bootloader left it.
- **lbs_hidl_service, LocationEM, mtklogger, factory/meta modes**.

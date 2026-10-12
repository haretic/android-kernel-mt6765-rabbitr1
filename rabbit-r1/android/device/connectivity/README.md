# MediaTek MT6765 CONSYS connectivity (Wi-Fi + Bluetooth)

Android side of the mainline kernel port in `drivers/staging/mtk-connectivity`
(kernel branch `rabbit-r1/connectivity`). The CONSYS is a CONNAC 1 "soc1_0"
block: BTIF carries MediaTek STP to the CONSYS MCU, WMT powers it and loads
its ROM patches, gen4m drives Wi-Fi over the AXI WFDMA, and `btmtk_wmt`
exposes Bluetooth as a standard `hci0`.

None of the stock MediaTek userspace (wmt_loader, wmt_launcher, wlan_assistant,
libbluetooth_mtk, the MTK Wi-Fi HAL) is needed. WMT answers its own patch
queries in the kernel (`CONFIG_MTK_CONN_WMT_AUTOLAUNCH`).

## Using it

Not picked up by the `optional/*/product.mk` glob. In the device tree:

    # device.mk
    $(call inherit-product, device/mainline/common/optional/connectivity/connectivity.mk)
    # BoardConfig.mk
    include device/mainline/common/optional/connectivity/BoardConfigConnectivity.mk

Put the five modules next to `TARGET_PREBUILT_KERNEL`, so that
`optional/prebuilt_kernel` installs them in `/vendor/lib/modules`:
`mtk_btif_drv.ko wmt_drv.ko wmt_chrdev_wifi.ko wlan_drv_gen4m.ko btmtk_wmt.ko`.

## Firmware

Copied unmodified from the stock `/vendor/firmware` into
`$(MTK_CONSYS_FIRMWARE_DIR)` (default `vendor/rabbit/r1-consys/proprietary/firmware`):

| File | Used by |
|------|---------|
| `WMT_SOC.cfg` | WMT (co-clock, PSM settings). wmt_drv refuses to start without it |
| `soc1_0_patch_mcu_1_1_hdr.bin` | WMT, MCU ROM patch (srh_patch) |
| `soc1_0_ram_mcu_1_1_hdr.bin` | WMT, WMT RAM code (srh_rom_patch) |
| `soc1_0_ram_bt_1_1_hdr.bin` | WMT, BT RAM code |
| `soc1_0_ram_wifi_1_1_hdr.bin` | WMT, Wi-Fi RAM code (ROM part) |
| `WIFI_RAM_CODE_soc1_0_1_1.bin` | gen4m, Wi-Fi firmware |

The kernel finds them through `firmware_class.path=/vendor/firmware/`
(BoardConfigMainlineCommon.mk). `BT_FW.cfg` is only read by the MTK BT HAL and
is not needed.

## Module order (init.mtk-consys.rc)

    mtk_btif_drv -> wmt_drv -> wmt_chrdev_wifi -> wlan_drv_gen4m -> btmtk_wmt

Each module uses the exports of the previous ones. wmt_drv must load after
`/vendor` is mounted. It checks `WMT_SOC.cfg` and the SoC chip id at load. The
CONSYS itself is powered on lazily, with the first function (Wi-Fi, BT or
loopback). `btmtk_wmt` registers `hci0`, and the Bluetooth core powers it on
once at registration (BT patch download), then off again after 2 s.

## Wi-Fi

`/dev/wmtWifi` (wmt_chrdev_wifi) is the power switch: `1` powers the Wi-Fi
function on, and gen4m probes and registers `wlan0` before the write returns.
`0` powers it off. `S`/`P`/`A` additionally switch to STA+P2P or AP mode
(p2p0/ap0), as the stock HAL does.

`MTK_CONSYS_WIFI_POWER` selects who writes it:

* `boot` (default): the `vendor.mtk-consys-wifi-on` service writes `1` once
  at boot. wlan0 stays up, and the framework runs without a Wi-Fi HAL
  (STA through wificond and wpa_supplicant). This is the simplest path for
  bring-up.
* `hal`: `android.hardware.wifi-service` with the legacy-HAL fallback.
  libwifi_hal writes `WIFI_DRIVER_STATE_ON/OFF` (1/0) to
  `WIFI_DRIVER_STATE_CTRL_PARAM` (/dev/wmtWifi) when Wi-Fi is toggled, so
  CONSYS Wi-Fi is really off when disabled. Soft AP is not wired: it would
  need the `A` write.

wpa_supplicant comes from `optional/wpa_supplicant_8` (APEX, nl80211). There
is no `BOARD_WPA_SUPPLICANT_PRIVATE_LIB`, so MediaTek `DRIVER` private
commands are unsupported. Android does not need them for STA.

NVRAM: gen4m takes calibration and the MAC address from the stock NVRAM
record (`/mnt/vendor/nvdata/APCFG/APRDEB/WIFI` on stock, not mounted on this
build). Without it gen4m runs on firmware defaults, and the MAC is generated
(`00:08:22:xx:xx:xx`, changes per boot). The record can be fed after
wlan_drv_gen4m is loaded and before the first `1`, as one write() of
`"WR-BUF:NVRAM"` followed by the record, e.g.

    { printf 'WR-BUF:NVRAM'; cat WIFI; } > /data/local/tmp/nv
    dd if=/data/local/tmp/nv of=/dev/wmtWifi bs=8192 count=1

## Bluetooth

`btmtk_wmt` is an H:4 hci_dev over the STP BT channel (the data path of the
stock `/dev/stpbt`). The default AIDL HAL (`TARGET_BLUETOOTH_HAL=default-aidl`,
on by default in options.mk) finds it through MGMT and opens it with the HCI
user channel. Its policy (`bluetooth_socket`, `net_admin`, `rfkill_device`)
is already in AOSP and mainline/common.

Address: the stock HAL programs one from NVRAM on every power on. btmtk_wmt
logs `controller address ...` in setup. If the address reads as zero, it
marks the controller unconfigured (`HCI_QUIRK_INVALID_BDADDR`). The
`set_bdaddr` service (package `bdaddr`, from hardware/mainline) then sets one
through MGMT; btmtk_wmt implements the MediaTek 0xfc1a set-address command.

## SELinux (sepolicy/)

* `wmtwifi_device` for `/dev/wmtWifi`; init chowns it to wifi:wifi 0660.
* `mtk_consys_wifi_on` domain for `/vendor/bin/mtk-consys-wifi-on.sh`.
* `hal_wifi_default` may write `/dev/wmtWifi` (used with `MTK_CONSYS_WIFI_POWER=hal`).
* Module loading needs nothing new: init already loads `/vendor/lib/modules`.
* `/proc/driver/wmt_dbg` (debug only) is not labeled; use it from a root
  shell on a permissive or userdebug build.

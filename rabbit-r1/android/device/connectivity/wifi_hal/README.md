# libwifi-hal-r1

Legacy Wi-Fi vendor HAL for the R1 (MT6765 CONSYS, gen4m on the mainline
kernel), used with `MTK_CONSYS_WIFI_POWER=hal`. `android.hardware.wifi-service`
`dlopen()`s it through `/vendor/etc/wifi/vendor_hals/libwifi-hal-r1.xml`
(primary HAL), so `BOARD_WLAN_DEVICE` stays unset and nothing in
`frameworks/opt/net/wifi` changes. Background and alternatives:
`/rabbitr1/tmp/wifi-vendor-hal.md`.

The base is the goldfish `libwifi-hal-emu`
(`device/generic/goldfish/wifi/wifi_hal`, Apache-2.0). On top of it the HAL
sends a few GOOGLE_OUI (0x001A11) `NL80211_CMD_VENDOR` commands that gen4m
implements (`os/linux/gl_vendor.c`, table `mtk_wlan_vendor_ops` in
`os/linux/gl_init.c`).

## Files

| File | Origin |
|---|---|
| `halstate.{h,cpp}` | goldfish, unchanged |
| `info.{h,cpp}` | goldfish, owns `EventLoop` + `Nl80211` instead of the rtnetlink `Netlink` |
| `eventloop.{h,cpp}` | goldfish `netlink.{h,cpp}` without the rtnetlink socket; `read()` instead of `recv()` on the pipe |
| `interface.{h,cpp}` | goldfish, emulator stubs dropped, gen4m vendor commands added |
| `wifi_hal.cpp` | goldfish, only implemented entry points are registered |
| `log.h` | goldfish, `LOG_TAG` changed |
| `nl80211.{h,cpp}` | new, synchronous libnl `NL80211_CMD_VENDOR` client |
| `gen4m_vendor.h` | new, gen4m subcommand and attribute IDs |
| `libwifi-hal-r1.xml` | new, vendor HAL descriptor |

## What is implemented

| Entry point | Implementation |
|---|---|
| `wifi_initialize`, `wifi_cleanup`, `wifi_event_loop` | goldfish `HalState`: `wifi_event_loop` blocks until `wifi_cleanup`; `wifi_cleanup` waits until the loop has run its stop callback, then calls the cleaned-up handler on the calling thread |
| `wifi_get_ifaces`, `wifi_get_iface_name` | goldfish: `wlan0`/`wlan1` if present in `/sys/class/net`, rescanned on every `wifi_initialize` |
| `wifi_get_supported_feature_set` | `0x0002` GET_FEATURE_SET, raw u64 reply. Bits backed by stubbed entry points (RSSI monitor, firmware roaming, TX power limit, ...) are masked, and `WIFI_FEATURE_INFRA` is added. Falls back to `WIFI_FEATURE_INFRA` with success if the command fails, because an error here fails `IWifiChip::getFeatureSet` |
| `wifi_set_country_code` | `0x0006` SET_COUNTRY_CODE, attribute 8 (2 chars + NUL) |
| `wifi_get_valid_channels` | `0x0001` GET_CHANNEL_LIST, attribute 1 (band) in, 2/3 (count, u32 MHz list) out. One query per `wifi_band` bit, because gen4m's band 3 means 6 GHz and not `WIFI_BAND_ABG`. Fixes the goldfish bug of copying `*numChannels` bytes instead of elements |
| `wifi_get_driver_version`, `wifi_get_firmware_version` | `0x1401` LOGGER_GET_VER with attribute 1 / 2, bare string reply |
| `wifi_get_link_stats` | `0x1200` LSTATS GET_INFO, see below |
| `wifi_set_link_stats` | no-op success: gen4m has no start command, the firmware always fills the stats buffer |

Everything else, including `wifi_wait_for_driver_ready` and
`wifi_virtual_interface_create`, stays on the service's `NOT_SUPPORTED`
stubs (`wifi_legacy_hal_stubs.cpp`). The service accepts both: it only fails
on `TIMED_OUT`/`UNKNOWN` from the wait, and on `NOT_SUPPORTED` from the
create when `wlan0` already exists. The goldfish emulator fakes (APF in
4 KiB of RAM, dropped keepalive packets, logger ring "ring0", fake "1.0"
versions, rtnetlink packet counters as link stats) were removed rather than
carried over to real hardware.

Link layer stats: the gen4m reply (`gl_vendor.c:1438-1454`) is
`wifi_iface_stat`, then `num_peers` x (`wifi_peer_info` + `num_rate` x
`wifi_rate_stat`), then a u32 `num_radios`, then the radios (`wifi_radio_stat`
+ `num_channels` x `wifi_channel_stat`). gen4m builds it from its own copies
of the legacy structures (`STATS_LLS_*` in `gl_vendor.h`), with the same
members in the same order, so the layout matches the Android 17
`link_layer_stats.h` on an arm64 kernel with 64-bit userspace. `interface.cpp`
pins the sizes with `static_assert`s, bounds-checks every count against the
reply length, copies into 8-byte aligned buffers, sets `iface`, and clears
`num_tx_levels`/`tx_time_per_levels` (the driver copies a firmware pointer
value there). The result goes to `on_link_stats_results`, the legacy
single-link callback. gen4m only reports `WIFI_FEATURE_LINK_LAYER_STATS`
when the firmware stats EMI buffer is mapped; without it the command fails
and the framework receives no stats. The library is built
`compile_multilib: "first"`, and a 32-bit build would compile LLS out.

## Wiring

`connectivity.mk` adds this namespace and installs `android.hardware.wifi-service`,
`libwifi-hal-r1` and `libwifi-hal-r1.xml` (`/vendor/etc/wifi/vendor_hals/`)
in both `MTK_CONSYS_WIFI_POWER` modes. `BOARD_WLAN_DEVICE` stays unset;
in `hal` mode driver power still goes through `wifi_load_driver()` in
libwifi_hal. `connectivity/sepolicy` lets the service set
`vendor.wlan.{driver,firmware}.version`; generic netlink and `net_admin`,
which `NL80211_CMD_VENDOR` needs, come from `system/sepolicy`.

## Checks on the device

* `logcat -s android.hardware.wifi-service wifi_hal_r1`: "processing config
  file: libwifi-hal-r1.xml", no "failed to open vendor hal library", and
  "Legacy HAL start complete".
* Toggle Wi-Fi off/on several times. There must be no "event loop terminated,
  but HAL was not stopping" and no "Legacy HAL stop failed or timed out".
* `getprop vendor.wlan.driver.version` / `vendor.wlan.firmware.version`
  (needs the sepolicy above).
* The supported feature set in `dumpsys wifi` must include 5 GHz. INFRA_5G
  comes from the NVRAM (`ucSupport5GBand`); if it is missing, the 5 GHz
  NVRAM/regulatory data is the problem, not the HAL.
* `cmd wifi get-country-code` after `cmd wifi force-country-code enabled DE`,
  and `iw reg get` if available: gen4m's own country (set by this HAL) and
  cfg80211's (set by wpa_supplicant) must agree.
* Link stats: `dumpsys wifi` "WifiLinkLayerStats" while connected. If the
  framework sees none, check dmesg for "EMI mapping not done" or the
  `kalIoctl` warning in `mtk_cfg80211_vendor_llstats_get_info`. To compare
  values, enable gen4m's `fgLinkStatsDump` and match its dump against
  `dumpsys`.

## Risks

* `wlan0` must exist when the service calls `wifi_initialize` (right after
  `wifi_load_driver()` and `SetWifiUpState(true)`). If gen4m registers it
  asynchronously after the `/dev/wmtWifi` write, start fails with "Failed to
  retrieve wlan interface handle". The fix would be a `wifi_wait_for_driver_ready`
  that polls `/sys/class/net/wlan0` and returns SUCCESS (TIMED_OUT is fatal).
* Link stats trust gen4m's copy of the firmware EMI layout
  (`HAL_LLS_FULL_REPORT`). If the firmware writes a different layout, the
  bounds checks keep the HAL from crashing, but the counters would be wrong.
  The framework polls link stats periodically while connected, and each
  poll is a firmware query.
* The country code set through the vendor command
  (`wlanoidSetCountryCode`) and the one from cfg80211 regulatory can
  disagree on the mainline kernel. Check the effective channel list after
  roaming between countries.
* Vendor commands time out after 5 s (`SO_RCVTIMEO`). After a timeout the
  socket is recreated, so a late reply cannot be matched to the next
  request.
* Soft AP / P2P interface creation is not covered:
  `wifi_virtual_interface_create` is a stub, and no concurrency matrix is
  reported, so the service keeps its static interface combinations.
* Only built here with a host clang `-fsyntax-only -Wall -Wextra -Werror`
  check (x86_64 LP64, where the `static_assert`ed sizes are the same as on
  arm64). It has not been through a Soong build.

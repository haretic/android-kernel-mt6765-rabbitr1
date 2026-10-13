# r1 thermal HAL

Android gets its temperatures from LineageOS's copy of Linaro's generic AIDL
thermal HAL, `android.hardware.thermal-service.linaro-generic` from
`hardware/linaro/libpm` (branch `lineage-24.0`). device/mainline/common already
supports it as `TARGET_THERMAL_HAL := linaro-libpm`
(`optional/thermal-hal_linaro-libpm`): that adds the package, the HAL's own
sepolicy (`hardware/linaro/libpm/sepolicy/thermal`) and the mainline common
policy for it. The HAL ships its init service and its VINTF fragment
(`android.hardware.thermal` AIDL version 1, which compatibility matrix 8 lists;
the r1 manifest has target-level 7).

The HAL does not read sysfs. It talks to the kernel over the thermal generic
netlink family (`libpm_libthermal`, on libnl): it discovers zones and cooling
devices by name, reads temperatures on demand, and forwards the kernel's
trip-crossed events to the framework callbacks. The kernel needs
`CONFIG_THERMAL_NETLINK=y`, which the `rabbit-r1/thermal` branch adds to
`rabbit_r1_defconfig`. The HAL's domain only needs `netlink_generic_socket`,
which libpm's policy grants, so the r1 needs no thermal policy of its own.

## Files

- `product.mk`: selects the HAL and installs `thermal.json`.
- `thermal.json`: installed as `/vendor/etc/thermal.json`, the HAL's default
  configuration (`vendor.thermal.config` can name another file in
  `/vendor/etc`). It maps kernel thermal zones and cooling devices to AIDL
  types and gives the thresholds for each throttling severity.

| Zone | Type | Sensor | Thresholds (C) |
|------|------|--------|----------------|
| `soc-thermal` | SOC | hottest of TS1-TS5 | Critical 95, Shutdown 110 |
| `cpu-big-thermal` | CPU | TS1, cores 0-3 | Severe 85, Shutdown 110 |
| `cpu-little-thermal` | CPU | TS2, cores 4-7 | Severe 85, Shutdown 110 |
| `gpu-thermal` | GPU | TS3 | Shutdown 110 |
| `ap-ntc-thermal` | SKIN | board NTC on AUXADC channel 0 | Light 45, Severe 53 |
| `mt6357-battery` | BATTERY | MT6357 gauge NTC | Shutdown 60 |

The thresholds match the kernel trip points (`mt6765.dtsi`,
`mt6765-rabbit-r1.dts`), because this HAL only computes a severity when the
kernel reports a trip crossing. The kernel throttles the CPUs from 85 C (per
cluster) and 95 C (whole SoC) and shuts down at 110 C. The board NTC has two
trips with no cooling device; they exist only to report the stock policy's
charge limit (45 C) and camera-off point (53 C) to the framework, which takes
its thermal status from SKIN sensors. The battery zone has no trips, so its
60 C (stock `tzbattery` reset) is only reported as a static threshold.

The board NTC and the battery zone are `Optional`: the HAL drops them if they
are missing instead of failing to start. `cpufreq-cpu4` is optional because
with `maxcpus=1` the second cluster has no cpufreq policy and therefore no
cooling device. The four SoC zones and `cpufreq-cpu0` are required: without
them the thermal driver did not probe, and the HAL failing to start says so.

## Hooking it in

1. Sync the HAL. `rabbit-r1/android/local_manifests/r1.xml` now pins it:

   ```xml
   <project path="hardware/linaro/libpm" name="LineageOS/android_hardware_linaro_libpm" remote="github" revision="d062161c81d3d2b61b27425c2e39c728eab7f9f0" clone-depth="1" />
   ```

   Copy the manifest to `/rabbitr1/src/android/.repo/local_manifests/r1.xml`
   and `repo sync hardware/linaro/libpm`.

2. Copy this directory to `device/rabbit/r1/thermal/`.

3. In `device/rabbit/r1/device.mk`, next to the health include and before
   `$(call inherit-product, device/mainline/common/mainline_common.mk)`:

   ```make
   include device/rabbit/r1/thermal/product.mk
   ```

   Use `include`, not `inherit-product`: `TARGET_THERMAL_HAL` must be set
   when `mainline_common.mk` includes `optional/*/product.mk`.

4. `BoardConfig.mk` needs no change. `BoardConfigMainlineCommon.mk` includes
   `optional/thermal-hal_linaro-libpm/board.mk`, which adds the sepolicy
   directories when `TARGET_THERMAL_HAL` is `linaro-libpm`, as it already does
   for `TARGET_GRAPHICS_COMPOSER_HAL` set in `device.mk`.

5. Use a kernel and DTB from the `rabbit-r1/thermal` branch (thermal zones,
   `CONFIG_THERMAL_NETLINK=y`, `CONFIG_SENSORS_NTC_THERMISTOR=y`).

## Checking it on the device

```sh
service list | grep android.hardware.thermal.IThermal/default
ps -A | grep thermal-service.linaro
logcat -s android.hardware.thermal-service.linaro-generic
dumpsys thermalservice
```

`dumpsys thermalservice` should list the six temperatures with their types
under "Current temperatures from HAL" and the cooling devices under "Current
cooling devices from HAL". A HAL that exits with "ThermalHAL missing required
sensor" means a required zone above is missing; compare with
`cat /sys/class/thermal/thermal_zone*/type`.

To see a severity change without heating the device, use the kernel's
emulated temperature (`CONFIG_THERMAL_EMULATION=y`) as root:

```sh
z=$(grep -l '^cpu-big-thermal$' /sys/class/thermal/thermal_zone*/type)
echo 86000 > ${z%/type}/emul_temp   # crosses the 85 C trip
dumpsys thermalservice | grep -A10 'Cached temperatures'
echo 0 > ${z%/type}/emul_temp
```

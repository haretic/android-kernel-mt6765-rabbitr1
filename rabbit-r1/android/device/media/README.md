# MT6765 hardware video codec

Hardware H.264 encode, then H.264/HEVC decode, through the stock MediaTek
Codec2 HAL running on the mainline kernel. Nothing here has run on an r1 yet.
The kernel side builds. The Android side is files and instructions only: it
has not been through an Android build.

## How it fits together

MT6765 has one video codec core at 0x17020000 (VENC) and 0x17040000 (VDEC).
Its H.264 encoder and its H.264/HEVC/MPEG-4 decoder take turns on it through
`VCODEC_SEL` in vcodecsys (0x17000000). Both reach memory through the VENC
ports of SMI larb1. No firmware is involved. On stock
(`drivers/misc/mediatek/videocodec/mt6765`), the kernel only does the
following:

- arbitrates the core between codec instances
- powers and clocks it
- routes `VCODEC_SEL`
- turns the done interrupts into `VCODEC_WAITISR` wakeups
- lets userspace `mmap()` the register windows

The 32-bit stock libraries parse the bitstreams and program every register
themselves:

- `libvcodecdrv`
- `libvcodec_utility`
- `libh264enc_sa.ca7`
- `libh264dec_sa.ca7`
- `libHEVCdec_sa.ca7.android`

The libraries get M4U addresses for their buffers from `/dev/ion`
(`ION_MM_CONFIG_BUFFER`, then `ION_SYS_GET_PHYS`).

There are no register documents, so neither the upstream mtk-vcodec driver
(VPU/SCP firmware SoCs only) nor a V4L2 stateful driver for v4l2_codec2 is
an option. The kernel therefore provides the stock ABI:

- `drivers/staging/mtk-vcodec-compat`: `/dev/Vcodec`, with the stock ioctls
  for native and 32-bit callers. It is enabled by `CONFIG_MTK_VCODEC_COMPAT=y`
  in `rabbit_r1_defconfig` and bound by the `video-codec@17020000` node.
- `drivers/staging/mtk-ion-compat`: the codec driver registers stock ports
  8-12 and 15-18 (`VENC_RCPU`..`VENC_RD_COMV`, `VENC_CUR_LUMA`..`VENC_REF_CHROMA`)
  with its IOMMU device. A buffer tagged for one of those ports gets its larb1
  M4U address from `GET_PHYS`/`GET_IOVA`, as on stock.

Android runs the stock `android.hardware.media.c2@1.2-mediatek` (HIDL
`IComponentStore/default`) as a 32-bit process. Its `c2.mtk.*` components run
on `libvcodecdrv`, because `mtk_platform_codecs_config.xml` says
`driverIntf="proprietary"`. On this device the framework uses HIDL Codec2
services, because `ro.vendor.api_level` is below 202404 and
`media.c2.hal.selection` is unset. They sit next to the swcodec APEX's HIDL
`IComponentStore/software`.

## Files

| File | Purpose |
|------|---------|
| `media.mk` | Product side: blobs, shim, 32-bit `.vendor` platform libraries, rc, ueventd, codec list, VINTF fragment. |
| `BoardConfigMedia.mk` | Board side: sepolicy dir, `BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES`. |
| `blobs.txt` | The 57 files to import from `out/stock-vendor/tree`: 3 configs and 54 32-bit ELFs. |
| `shim/` | `libr1_codec2_shim.so` (32-bit) provides the five Android 12 symbols the blobs import and Android 17 lacks. See "Linking" below. |
| `init.r1.media.rc` | The HAL service: user `mediacodec`, stock groups without the network ones. |
| `ueventd.r1-media.rc` | `/dev/Vcodec 0660 media system`, as stock. |
| `media_codecs_c2_encode.xml` | Stage 1 codec list: `c2.mtk.avc.encoder` only. Installed as `/vendor/etc/media_codecs.xml` (the optional FFmpeg HAL owns `media_codecs_c2.xml`). |
| `media_codecs_c2.xml` | Stage 2 codec list: adds `c2.mtk.avc.decoder` and `c2.mtk.hevc.decoder`. |
| `manifest-media.xml` | HIDL `android.hardware.media.c2@1.2::IComponentStore/default`. |
| `sepolicy/` | The HAL runs in the platform `mediacodec` domain. Adds `/dev/Vcodec` (`r1_vcodec_device`), `/dev/ion` and GED for that domain. |

Both codec lists rank the hardware codecs 256, ahead of `c2.android.*` at 512.
The framework only lists HAL components named in the list, so the HAL's other
components stay hidden: MPEG-4/H.263/VP8/VP9 and the MediaTek audio decoders.

## Linking

The dependency closure was resolved with `DT_NEEDED` plus the libraries the
blobs `dlopen()`. Each name was looked up in stock `lib/`, then in the 32-bit
`vendor/lib` and `system/lib` of `out/target/product/r1`. Every library
resolves. Every import is defined somewhere in the closure except these five,
all of which `libr1_codec2_shim` provides:

| Symbol | Imported by | Android 17 replacement |
|--------|-------------|------------------------|
| `android::base::WriteStringToFd(const std::string&, borrowed_fd)` | `libcodec2_hidl@1.0/1.1/1.2`, `libavservices_minijail_vendor` | `WriteStringToFd(std::string_view, borrowed_fd)` |
| `GraphicBufferMapper::lock(handle, uint32_t, const Rect&, void**, int32_t*, int32_t*)` | `libcodec2_vndk` | `lock(handle, int64_t, const Rect&)` returning `LockResult` |
| `GraphicBufferMapper::unlock(handle)` | `libcodec2_vndk` | `unlock(handle, nullptr)`, which still waits for the fence |
| `GraphicBufferSource::getHGraphicBufferProducer() const` | `libcodec2_hidl@1.0/1.1/1.2` | `B2HGraphicBufferProducer` over `getIGraphicBufferProducer()` |

The shim was compiled with the tree's clang and the 32-bit
`libstagefright_bufferqueue_helper` flags plus `-DNO_BINDER`. It defines
exactly these mangled names. Its own imports all exist in the 32-bit vendor
`libui` and `libbase`.

The stock seccomp policies ship unchanged. Their syscall set is a superset of
Android 17's own `android.hardware.media.c2-default-arm.policy`.

The blobs also define some symbols that platform libraries define too:
compiler-rt helpers, libc++ template instantiations and HIDL passthrough
templates. Two of these are worth knowing about:

- `libcodec2_vndk` carries its own copies of the bufferqueue `b2h` converters.
- `libladder` exports the `unwindstack::LocalMaps` vtable. The 64-bit PowerVR
  stack already ships the same `libladder`.

## Wiring (edits to existing files, left to you)

1. **Kernel**: build `rabbit-r1/vcodec` with `scripts/build-mainline.sh`. The
   defconfig and DT changes are on that branch. Then reinstall the device
   with `scripts/install-lineage-device.py`, which also copies this `media/`
   directory to `device/rabbit/r1/media`.
2. **Blobs**: create `vendor/rabbit/r1-media/proprietary/` the same way as
   `vendor/rabbit/r1-pvr`. In the Android tree:

   ```sh
   grep -v -e '^#' -e '^$' device/rabbit/r1/media/blobs.txt | while read -r f; do
       install -D -m 0644 /rabbitr1/out/stock-vendor/tree/$f vendor/rabbit/r1-media/proprietary/$f
   done
   for f in libcodec2_hidl@1.0.so libcodec2_hidl@1.1.so libcodec2_hidl@1.2.so \
            libcodec2_vndk.so libavservices_minijail_vendor.so; do
       patchelf --add-needed libr1_codec2_shim.so vendor/rabbit/r1-media/proprietary/lib/$f
   done
   ```

   Check each patched library with `readelf -d`: it should list
   `libr1_codec2_shim.so` as `NEEDED`.

   Then move the Android 12 libraries whose names AOSP modules own into
   `lib/r1-c2/` and give every ELF a runpath to it (see `blobs.txt`):

   ```sh
   cd vendor/rabbit/r1-media/proprietary
   mkdir -p lib/r1-c2
   for f in android.hardware.media.c2@1.0.so android.hardware.media.c2@1.1.so \
            android.hardware.media.c2@1.2.so libavservices_minijail_vendor.so \
            libcodec2_hidl@1.0.so libcodec2_hidl@1.1.so libcodec2_hidl@1.2.so \
            libcodec2_hidl_plugin.so libcodec2_soft_common.so libcodec2_vndk.so \
            libhwbinder.so libsfplugin_ccodec_utils.so libstagefright_bufferpool@2.0.1.so; do
       mv lib/$f lib/r1-c2/$f
   done
   for f in $(find bin lib -type f); do
       head -c4 $f | grep -q ELF && patchelf --set-rpath /vendor/lib/r1-c2 $f
   done
   ```

   Installed under their own names in `/vendor/lib`, they would clash at build
   time with the AOSP modules of the same name ("overriding commands for
   target"). Bionic searches the caller's `DT_RUNPATH` for both `DT_NEEDED`
   and `dlopen()`, so the HAL finds them there.
3. **`device.mk`**: `$(call inherit-product, $(LOCAL_PATH)/media/media.mk)`.
   It requires `R1_GPU=pvr`, because buffers are mapped with the stock 32-bit
   PowerVR gralloc mapper.
4. **`BoardConfig.mk`**: `include $(DEVICE_PATH)/media/BoardConfigMedia.mk`.
5. **`rootdir/ueventd.r1.rc`**: `import /vendor/etc/ueventd.r1-media.rc`.
6. Keep `media.c2.hal.selection` unset (or `hidl`). Setting it to `aidl` hides
   this HAL.
7. Do not add `.vendor:32` packages for the names `blobs.txt` ships.
   `libcodec2_vndk`, `libcodec2_hidl@1.x`, `libsfplugin_ccodec_utils`,
   `libstagefright_bufferpool@2.0.1`, `libcodec2_soft_common`, `libhwbinder`
   and `android.hardware.media.c2@1.x` are the Android 12 copies the blobs
   were built against.

Set `R1_MEDIA_HW_DECODE := true` (before `media.mk` is inherited) to install the
stage 2 list.

## Stages

0. **Kernel.** `/dev/Vcodec`, core locking and power, interrupts, register
   windows, M4U addresses. Test with `rabbit-r1/tools/r1-vcodec-test.c`.
1. **H.264 encode** (default). Camera recording goes through the encoder input
   surface: gralloc buffers from `/dev/ion` are imported by the component and
   given larb1 addresses. Input that is already YUV 4:2:0 needs no conversion.
2. **H.264/HEVC decode** (`R1_MEDIA_HW_DECODE=true`).
   `mtk_platform_codecs_config.xml` sets `alwaysColorConvertChecker` and
   `ForceCCWhenSW`, so decoder output may be converted with `libdpframework`.
   That needs the MediaTek MDP and CMDQ devices (`/dev/mtk_cmdq`,
   `/dev/mdp_sync`), which this kernel does not have.

## Hardware test plan

Kernel, from a shell on the device:

```sh
dmesg | grep -iE 'mtk-vcodec-compat|17020000'          # no probe errors
ls -l /dev/Vcodec                                      # crw-rw---- media system
grep -i vcodec /sys/kernel/debug/pm_genpd/pm_genpd_summary  # domain off while idle
ls /proc/device-tree/chosen/ | grep atag,devinfo       # libmtk_drvb's chip check reads it
stop vendor.media.c2-hal-1-2                           # once Android runs it
./r1-vcodec-test32; ./r1-vcodec-test                   # 0 failure(s) each
./r1-ion-ged-test32                                    # still 0 failures
grep vcodec /proc/interrupts
```

`r1-vcodec-test` checks the following. It never starts an engine:

- the mmap windows the libraries use, including refusals
- `VCODEC_SEL` and the deblock and VDEC bits while locked
- double locks, try-locks, non-owner waits and unlocks
- decoder and encoder waits for a busy core
- release when the owner dies
- larb1 IOVAs for a scattered ION buffer tagged `VENC_RCPU` and `VENC_CUR_LUMA`

Android, stage 1:

```sh
lshal | grep media.c2                                  # @1.2::IComponentStore/default and /software
lshal debug android.hardware.media.c2@1.2::IComponentStore/default   # c2.mtk.* components
file /proc/$(pidof android.hardware.media.c2@1.2-mediatek)/exe       # 32-bit
logcat -b all | grep -iE 'mtk|c2|vcodec|linker|seccomp|avc: denied'
dumpsys media.player | grep -A3 'video/avc'           # c2.mtk.avc.encoder listed first
```

Record 1280x720 at 30 fps for 30 s in Aperture. Then check:

- `dumpsys media.metrics | grep -i 'mediacodec.codec'` names `c2.mtk.avc.encoder`.
- `ffprobe -count_frames -show_entries stream=nb_read_frames,avg_frame_rate,duration`
  on the pulled file shows about 30 fps.
- `top -m 10` shows the HAL well under one core, against about 2.4 cores for
  `c2.android.avc.encoder`.

`screenrecord --time-limit 10` exercises RGBA input, which the encoder may
convert through MDP, so treat a failure there separately from camera
recording.

Stage 2: play a 720p H.264 and a 1080p HEVC file. `media.metrics` should name
`c2.mtk.avc.decoder` and `c2.mtk.hevc.decoder`, and the logcat should show
no `DpBlitStream`/cmdq errors.

## Open points

- **Hardware behaviour is inferred, not observed.** The driver follows the
  stock driver's sequences: `VCODEC_SEL`, deblock and VDEC bits, DRAM drain,
  break and reset, level-low interrupts. Probe, power and the first lock are
  the first things to watch.
- **No EMI bandwidth or DVFS backend.** `SET_FRAME_INFO` and the EMI and CPU
  hints are accepted and ignored, so `mm_sel` stays at its boot rate. If 720p
  encode falls short of 30 fps, raise that clock first.
- **Android 12 C++ in an Android 17 process.** The symbol check is clean
  with the shim, but class layouts are not covered by it.
  `GraphicBufferMapper` and `AMessage`/`ABuffer` cross the boundary.
- **`libmtk_drvb`** scrambles its state when
  `/proc/device-tree/chosen/atag,devinfo` is missing. LK is expected to inject
  it into the mainline DT.
- **Colour conversion (MDP/CMDQ)** is missing. It blocks stage 2 if the
  decoders insist on it, and blocks encoder input that is not YUV 4:2:0.
- **Not supported:** secure video (`secure_inst`), the JPEG encoder (its
  locks are refused), and buffers from DMA heaps rather than `/dev/ion`.
- **sepolicy** covers the device nodes. Property reads of `vendor.mtk.*` will
  show up as denials in permissive mode; add them from the log.

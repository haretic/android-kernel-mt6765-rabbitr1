# MT6765 hardware video codec

Hardware H.264 encode, then H.264/HEVC decode, through the stock MediaTek
Codec2 HAL running on the mainline kernel.

Status: the HAL runs enforcing and creates `c2.mtk.avc.encoder`, and the
encoder core runs (VENC interrupts) under Aperture. Its output is still empty,
for the two kernel gaps in "Stages", so the codec lists are off by default
and the `c2.android` software codecs do the work.

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
`media.c2.hal.selection` is `hidl` (media.mk). They sit next to the swcodec APEX's HIDL
`IComponentStore/software`.

## Files

| File | Purpose |
|------|---------|
| `media.mk` | Product side: blobs, shim, 32-bit `.vendor` platform libraries, rc, ueventd, codec list, VINTF fragment. |
| `BoardConfigMedia.mk` | Board side: sepolicy dir, `BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES`. |
| `blobs.txt` | The 53 files to import from `out/stock-vendor/tree`: 2 configs and 51 32-bit ELFs. |
| `dpframework/` | `libdpframework.so` (32-bit) in place of the stock one: the `DpAsyncBlitStream` calls of the encoder and decoder, done with libyuv on the CPU. The stock library drives the MDP through `/dev/mtk_cmdq` and `/dev/mdp_sync`. |
| `shim/` | `libr1_codec2_shim.so` (32-bit) provides the five Android 12 symbols the blobs import and Android 17 lacks. See "Linking" below. |
| `init.r1.media.rc` | The HAL service: user `mediacodec`, stock groups without the network ones. |
| `ueventd.r1-media.rc` | `/dev/Vcodec 0660 media system`, as stock. |
| `media_codecs_c2_encode.xml` | Stage 1 codec list: `c2.mtk.avc.encoder` only. Installed as `/vendor/etc/media_codecs.xml` (the optional FFmpeg HAL owns `media_codecs_c2.xml`). |
| `media_codecs_c2.xml` | Stage 2 codec list: adds `c2.mtk.avc.decoder` and `c2.mtk.hevc.decoder`. |
| `seccomp_policy/` | The stock extended seccomp policy plus the crash handler's syscalls ("Linking"). |
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

The stock seccomp policies are a superset of Android 17's own
`android.hardware.media.c2-default-arm.policy`. The device ships its own copy
of the extended one, `seccomp_policy/`, with `getppid`, `sysinfo` and `uname`
added from `crash_dump.arm.policy`. The in-process crash handler makes these
calls, so without them any crash in the HAL ends in `SIGSYS` with no
tombstone. The whole file cannot be `@include`d: it filters `mmap2` and
`mprotect` too, and this minijail rejects a second filter for a syscall
("duplicate label").

Android 12 class layouts in an Android 17 process:

- **`AHandler`** (`libstagefright_foundation`). Android 17 appended `mLock`
  and the delivery status. The blobs' handler subclasses keep their own fields
  there, so Android 17's `deliverMessage()` locks garbage and the encoder
  thread spins. The HAL loads the Android 12 build from `prebuilts/vndk/v31`
  in `r1-c2/`. `r1-c2/libcodec2_vndk.so` is the first library to need it, so
  that copy is the one loaded. The process's Android 17
  `libstagefright_bufferqueue_helper` then uses it too. Its handlers only add
  to the end of the Android 12 layout, and only Android 12 code reads them.
- **`Fence`** (`libui`). Android 17 made `getSignalTime()` and the
  destructor virtual, so the object grew a vtable pointer: 12 bytes against
  Android 12's 8. `libcodec2_vndk` allocates the dequeue fences of
  `C2BufferQueueBlockPool` itself, so the Android 17 constructor wrote past
  them and `wait()` read the fd from beyond them ("sync_file_info returned
  NULL for fd 0"); decoding to a surface crashed the HAL with heap
  corruption. Its three `Fence` imports are renamed to `r1compt::Fence`,
  which the shim implements with the Android 12 layout (`blobs.txt`). The
  fd is a `unique_fd`, as in Android 12: the inline destructor closes it with
  that `unique_fd`'s fdsan tag.
- **`GraphicBuffer`** (`libui`). Android 17 appended a `DependencyMonitor`:
  the class went from 0xa0 to 0x6f8 bytes. `C2BufferQueueBlockPool` in
  `libcodec2_vndk` allocates the buffers it dequeues from a surface with
  `new`, so the constructors overwrote 1.6 KiB of heap per buffer and the
  decoder deadlocked or crashed on corrupted locks soon after. Its three
  allocations call the shim's `r1gbnew` in place of `operator new`, through
  the PLT slot of an import it never calls (`blobs.txt`). The members before
  the monitor are unchanged, so the code's inline accesses still fit.
  `GraphicBufferMapper` and `GraphicBufferAllocator` are 8 bytes in both.
  `GraphicBufferSource`, which `libcodec2_hidl` allocates at 0x120 bytes, is
  only built for HAL-side input surfaces, which the framework asks a HIDL
  HAL for only with `debug.stagefright.c2inputsurface` set.
- **`C2InterfaceHelper::GetBaseOffset`** (Android 12 `libcodec2_vndk`) maps
  the offset of a flexible array element to element 0, then keeps searching
  and returns "not found". `F(values[i])` for `i > 0` therefore gets a null
  field helper. Android 17 (all launch levels from T) sends
  `output.subscribed-indices` with several entries, and the stock
  `NonStrictValuesWithNoDeps` setter walks them, so `config()` crashed with a
  null dereference. AOSP T sidestepped this with a no-op setter
  (`SubscribedParamIndicesSetter`), and the framework only subscribes on
  T-launch devices. The fix here is in the function itself: a 2-byte branch
  change makes the primitive case return the reduced offset (`blobs.txt`).

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
6. `media.mk` sets `media.c2.hal.selection=hidl`: `aidl` hides this HAL.
   mainline common sets `aidl` as an overridable default (android patch
   0011).
7. Do not add `.vendor:32` packages for the names `blobs.txt` ships.
   `libcodec2_vndk`, `libcodec2_hidl@1.x`, `libsfplugin_ccodec_utils`,
   `libstagefright_bufferpool@2.0.1`, `libcodec2_soft_common`, `libhwbinder`
   and `android.hardware.media.c2@1.x` are the Android 12 copies the blobs
   were built against.

`R1_MEDIA_HW_DECODE` (default `true`) installs the stage 2 list;
`R1_MEDIA_HW_DECODE := false` with `R1_MEDIA_HW_ENCODE` (default `true`) the
stage 1 one. Set them before `media.mk` is inherited. With neither, the HAL
runs but the framework lists none of its components.

## Stages

0. **Kernel.** `/dev/Vcodec`, core locking and power, interrupts, register
   windows, M4U addresses. Test with `rabbit-r1/tools/r1-vcodec-test.c`.
   Done; both tests pass.
1. **H.264 encode** (`R1_MEDIA_HW_ENCODE`, on). Camera recording goes
   through the encoder input surface. Works: YUV and RGBA surface input
   decode clean (`rabbit-r1/tools/r1-venc-test`, 54 dB PSNR from YUV),
   38 fps at 720p from a surface against 25 for `c2.android.avc.encoder`,
   at half its CPU time. It took two fixes:
   - **Linear buffers come from DMA heaps.** mainline common sets
     `debug.c2.use_dmabufheaps=1`. The Android 12 `C2Store` reads it too, so
     the component's bitstream and working buffers come from
     `/dev/dma_heap/system`. `libmemory_c2` then asks `/dev/ion` for their M4U
     address (`ION_IOC_CUSTOM`), and `mtk-ion-compat` refused buffers it did
     not export (`EFAULT`). The encoder ended up writing to IOVA 0: larb1
     port 2 write faults and empty access units. Setting the property to 0
     would move every Android 17 Codec2 user to `/dev/ion` as well.
     `mtk-ion-compat` now takes foreign dma-bufs: it keeps their
     `CONFIG_BUFFER` port, maps them through the engine's device with
     `dma_buf_attach()`, and keeps the mapping for as long as the buffer
     lives, as stock keeps its M4U mappings.
   - **DMA heap names.** The framework makes the vendor store the
     preferred one for every Codec2 allocator, `media.swcodec` and
     `mediaserver` included, and MediaTek's store answers `mtk_mm` or
     `mtk_mm-uncached`. Once the store's parameters worked
     (`GetBaseOffset`), the software codecs failed every allocation
     (`fetch linear block err = 14`). `mtk-ion-compat` now registers both
     heaps (`ion_mm_heap` buffers, cached and not); `sepolicy/` and
     `ueventd.r1-media.rc` open them as the system heap.
   - **Colour conversion.** Aperture's frames reach the encoder as RGBA (its
     CameraX pipeline renders through GL). `C2MtkVencExt` converts them with
     `libdpframework`. The stock one needs `/dev/mdp_sync` and
     `/dev/mtk_cmdq`, and every frame failed with `DpSync: createFence error`
     / `ConvertFrame err = 14`. `dpframework/` replaces it: RGB, YUYV, I420,
     YV12, NV12 and NV21 in, I420, YV12, NV12 and NV21 out, with crop,
     bilinear scaling and rotation, BT.601, BT.709 or BT.2020, limited or full range. A
     job converts in `invalidate()`; `createJob` returns a signalled
     sync_file exported from an idle dma-buf. Colour values use the kernel's
     `MDP_COLOR` layout, as the stock libraries do (I420 is 0x6c0848).
     The encoder asks for input usage 0, which gets it GPU-only buffers that
     `mtk-ion-compat` maps write-combined; the CPU reads those at about
     50 MB/s, 9 fps at 720p. `blobs.txt` patches its default input usage to
     `CPU_READ`: the gralloc then allocates `ION_FLAG_CACHED`, and
     `DMA_BUF_IOCTL_SYNC` invalidates before the conversion reads.
2. **H.264/HEVC decode** (`R1_MEDIA_HW_DECODE`, on). The decoders write
   MediaTek's block format (NV12 in 16x32 tiles, V4L2's MM21), which
   `dpframework/` detiles with libyuv and converts. Output is bit-identical
   to ffmpeg's, from buffers and from surfaces (`rabbit-r1/tools/r1-vdec-test`).
   On screen, 1080p HEVC plays at 30 fps for one core of HAL time, against
   2.5 for `c2.android.hevc.decoder`. It took:
   - **The `mtk_mm` heaps** (stage 1) for the decoders' buffers.
   - **`DpRect`** is eight ints (each of x, y, w and h with a sub-pixel
     part), not four; with four, `dpframework/` read the height as 0 and
     scaled every frame.
   - **The chip check.** The HAL limits the decoders to 720p unless it can
     read `atag,devinfo` (`sepolicy/`).
   - **`Fence` and `GraphicBuffer`** in `libcodec2_vndk` (above) for surface
     output.
   - **Converted surface output.** The decoders hand block-format buffers
     (`HAL_PIXEL_FORMAT_NV12_BLK`) to surfaces, where the stock display
     hardware scans them out. The PowerVR cannot sample them, and
     SurfaceFlinger aborts ("Failed to create a valid texture").
     `vendor.mtk.c2.vdec.force.output.cc=1` (`media.mk`) makes the decoders
     convert surface output to YV12 too, as they already do for other
     consumers.

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

Android, stage 1 (the default build):

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
no `DpBlitStream`/cmdq errors. `r1-vdec-test` compares each decoder with
ffmpeg, from buffers and from a surface (expect `PSNR ... inf`), and its
`display` mode plays a clip on screen at its frame rate.

## Open points

- **Hardware behaviour is inferred from the stock driver.** `VCODEC_SEL`,
  deblock and VDEC bits, DRAM drain, break and reset, level-low interrupts
  follow its sequences. H.264 encode and decode run correctly on them.
- **No EMI bandwidth or DVFS backend.** `SET_FRAME_INFO` and the EMI and CPU
  hints are accepted and ignored, so `mm_sel` stays at its boot rate. If 720p
  encode falls short of 30 fps, raise that clock first.
- **Android 12 C++ in an Android 17 process.** The symbol check is clean
  with the shim, but class layouts are not covered by it. `AHandler`,
  `C2InterfaceHelper`, `Fence` and `GraphicBuffer` were wrong ("Linking").
  Classes the blobs allocate are compared by the size their `new` asks for;
  `GraphicBufferSource` (HAL-side input surfaces, off by default) is the one
  left unchecked.
- **`libmtk_drvb`** scrambles its state when
  `/proc/device-tree/chosen/atag,devinfo` is missing. LK is expected to inject
  it into the mainline DT. The codec HAL reads it under its own label
  (`sepolicy/`); without it the decoders stop at 720p.
- **Colour conversion** runs on the CPU (`dpframework/`), not on the MDP.
  10-bit and compressed (UFO) block formats are not taken. Surface output
  is detiled and converted in two passes; one would save about half the
  decoder HAL's CPU time.
- **Not supported:** secure video (`secure_inst`) and the JPEG encoder (its
  locks are refused).

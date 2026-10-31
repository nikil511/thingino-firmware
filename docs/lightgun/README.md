# Lightgun camera experiments (Sonoff CAM Slim Gen2)

Hardware: Sonoff CAM Slim Gen2 (`sonoff_s2_t23n_sc2336_atbm6012bx`):
Ingenic T23N, SmartSens SC2336 (2 MP, 2-lane MIPI), ATBM6012BX Wi-Fi.

Goal: find the fastest *useful* optical tracking rate this camera can deliver.
That means sensor readout rate, ISP output rate, processing rate and field of
view, measured separately.

| Document | Contents |
|---|---|
| [CAPTURE_BENCHMARK.md](CAPTURE_BENCHMARK.md) | `lgcap` tool, method, baseline 1080p results |
| [ISP_SCALING.md](ISP_SCALING.md) | ISP scaler experiment (ch0 scaled vs separate ch1) |
| [SC2336_60FPS.md](SC2336_60FPS.md) | 720p60 port, timing, validation |
| [SC2336_HIGH_FPS_RESEARCH.md](SC2336_HIGH_FPS_RESEARCH.md) | >60 fps analysis, experimental modes, measured limits |
| [SC2336_SOURCES.md](SC2336_SOURCES.md) | register tables and sources behind the research |
| [FOV_COMPARISON.md](FOV_COMPARISON.md) | field of view per mode, marker visibility |
| [RECOMMENDATION.md](RECOMMENDATION.md) | which mode to use |

Every result is labelled as one of:

- **[HW]** measured on the camera
- **[SRC]** read from source code or register tables
- **[CALC]** theoretical estimate
- **[UNTESTED]** a configuration that builds but hasn't run on hardware

## What's in the tree (branch `lightgun-highfps`)

- `package/ingenic-sdk/0004-sensor-t23-sc2336-selectable-modes.patch`: adds a
  `sensor_mode` module parameter to the T23 SC2336 driver. Mode 0 is the stock
  1080p table, unchanged. The other modes are listed in
  [SC2336_60FPS.md](SC2336_60FPS.md) and
  [SC2336_HIGH_FPS_RESEARCH.md](SC2336_HIGH_FPS_RESEARCH.md).
- `package/thingino-lightgun-bench/`: `lgcap` (native NV12 capture benchmark)
  and `lgbench` (runs the standard test matrix).
- The camera defconfig enables `BR2_PACKAGE_THINGINO_LIGHTGUN_BENCH`.

## Build (macOS, Apple Silicon)

The upstream `build-container.sh` doesn't work as-is on macOS with Podman 6.
The Podman VM's shared folders can't hold symlinks, which Buildroot needs, so
the build output has to live in a Podman volume inside the VM. One-time setup:

```sh
brew install podman
diskutil apfs addVolume disk3 "Case-sensitive APFS" thingino
podman machine init --cpus 8 --memory 10240 --disk-size 100 \
  -v /Users:/Users -v /private:/private -v /var/folders:/var/folders \
  -v /Volumes/thingino:/Volumes/thingino
podman machine start
# download cache: the thingino-dl image is amd64-only; it's data, so that's fine
podman pull --arch amd64 ghcr.io/themactep/thingino-dl:latest
podman create --name thingino-dl-cache ghcr.io/themactep/thingino-dl:latest /bin/true
podman create --name thingino-dl-cache-src ghcr.io/themactep/thingino-dl:latest /bin/true
SRC=$(podman inspect thingino-dl-cache --format '{{range .Mounts}}{{if eq .Destination "/dl"}}{{.Source}}{{end}}{{end}}')
podman machine ssh "podman export thingino-dl-cache-src | tar -x --strip-components=1 -C '$SRC' dl/"
podman rm thingino-dl-cache-src
podman run --rm --user 0 --volumes-from thingino-dl-cache \
  ghcr.io/themactep/thingino-builder-image:latest chmod -R a+rwX /dl
```

Build (incremental after the first run). `/Volumes/thingino/lgmake.sh` wraps
the `podman run` call:

```sh
/Volumes/thingino/lgmake.sh all                 # first build
```

Thingino only re-runs Buildroot when the image is out of date. Its
`br-<pkg>-dirclean` also wipes the whole `target/` tree. So after changing the
driver patch, use this verified sequence:

```sh
L=/Volumes/thingino/lgmake.sh
$L br-ingenic-sdk-dirclean                     # re-extract + re-patch the SDK
podman run --rm --userns=keep-id -v thingino-output:/workspace/output:U \
  ghcr.io/themactep/thingino-builder-image:latest sh -c \
  'find /workspace/output/lightgun-highfps/*/build -maxdepth 2 -name .stamp_target_installed -delete'
$L br-all                                      # reinstall everything into target/
podman run --rm --userns=keep-id -v thingino-output:/workspace/output:U \
  ghcr.io/themactep/thingino-builder-image:latest sh -c \
  'rm -f /workspace/output/lightgun-highfps/*/images/thingino-*.bin*'
$L pack
```

After changing only `lgcap.c`: run `$L br-thingino-lightgun-bench-rebuild br-all`,
remove the old image as above, then `$L pack`.

Copy the image out of the volume:

```sh
podman run --rm --userns=keep-id -v thingino-output:/o:ro -v /Volumes/thingino/firmware:/f \
  ghcr.io/themactep/thingino-builder-image:latest \
  sh -c 'cp /o/lightgun-highfps/*/images/thingino-*.bin* /f/'
```

## Deploy

### On a camera already running thingino (how the measurements were made)

No flashing needed. Everything runs from the SD card and the stock module
comes back after a reboot.

1. **Build the sensor module against the camera's own ingenic-sdk revision.**
   A module built from this branch's SDK won't match an older camera's
   `tx-isp-t23.ko` (the sensor-registry ABI changed). Find the camera's
   thingino commit in `/etc/os-release` (`BUILD_ID`), and the
   `INGENIC_SDK_VERSION` at that commit. Then build `sc2336.c` in that SDK
   tree. For the test camera (`ciao+2c3eca5` → SDK `5e5d7d2`), the ported
   driver is `tools/sc2336-ingenic-sdk-5e5d7d2.c`:

   ```sh
   # inside the builder container, SDK checked out at 5e5d7d2 in $W, kernel tree $L
   cp tools/sc2336-ingenic-sdk-5e5d7d2.c $W/3.10.14/sensor-src/t23/sc2336.c
   make -C $L M=$W ARCH=mips CROSS_COMPILE=$B/host/bin/mipsel-linux- \
     SOC_FAMILY=t23 KERNEL_VERSION=3.10.14 SENSOR_1_MODEL=sc2336 \
     EXTRA_CFLAGS=-DCONFIG_KERNEL_3_10 modules
   ```

   Check the vermagic against the stock module (`strings *.ko | grep vermagic`).
   Also check that `nm -u` lists no symbols the stock module doesn't have.
2. **Copy to the SD card.** dropbear has no SFTP, so use `scp -O`:

   ```sh
   scp -O lgcap lgbench sensor_sc2336_t23.ko root@CAMERA:/mnt/mmcblk0p1/lightgun/bin/
   ```

3. **Swap the module and select a mode** (prudynt must be stopped first):

   ```sh
   /etc/init.d/S32prudyntwd stop; /etc/init.d/S31prudynt stop
   rmmod sensor_sc2336_t23
   insmod /mnt/mmcblk0p1/lightgun/bin/sensor_sc2336_t23.ko sensor_mode=1
   dmesg | grep "sc2336 mode"       # sc2336 mode 1: 1280x720 hts 1800 vts 750 max 60 fps
   ```

4. **Measure.** `lgbench` runs the full matrix for the loaded mode.
   `tools/lgmodes.sh MODE "FPS..."` smoke-tests rates, and `tools/lgfast.sh`
   runs the workload/stability set. Results go to the SD card; never use
   `/tmp`, which is RAM. Raw `.nv12` dumps fill a 128 MB card quickly, so copy
   results off and delete the dumps.
5. **Recover:** `reboot`. That reloads the stock module from flash.

### Fresh install of this branch

1. Flash `thingino-sonoff_s2_t23n_sc2336_atbm6012bx.bin` with the method for
   this camera on thingino.com. **Don't `sysupgrade` across bootloader
   layouts**: the test camera's boot partition is 256 kB, this build's is
   320 kB. Flash size is 16 MB; thingino.com's "8 MB" is wrong.
2. Select a mode persistently, then reboot:

   ```sh
   echo "sensor_sc2336_t23 sensor_mode=1" > /etc/modules.d/30-sensor
   reboot
   ```

3. `lgcap`, `lgbench` and the patched module are already in the image.

### Mode cheat sheet (all measured, see [SC2336_HIGH_FPS_RESEARCH.md](SC2336_HIGH_FPS_RESEARCH.md))

| `sensor_mode` | Output | Max fps | FOV |
|---|---|---|---|
| 0 | 1920×1080 | 32 | full |
| 1 | 1280×720 | 60 | 2/3 × 2/3 |
| 2 | 1920×540 | 63 | 1 × 1/2 |
| 3 | 960×540 | 78 | 1/2 × 1/2 |
| 4 | 640×480 | 88 | 1/3 × 4/9 |
| 5 | 640×360 | 115 | 1/3 × 1/3 |
| 6 | 1920×360 | 92 | 1 × 1/3 |
| 7 | 960×540 bin | — | corrupt image, research only |

Heights that aren't multiples of 8 (540) must be read through the ISP scaler
(`-o 1920x536`, `-o 960x536`), because libimp rejects them as channel sizes.

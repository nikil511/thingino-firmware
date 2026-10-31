# Capture benchmark (Phase 1)

Status: tool compiles for the camera (MIPS32 uClibc, 22 kB) and ships in the image **[UNTESTED]** on hardware. No measurements on the camera yet. Every
table below that's marked *pending* needs a run on the camera.

## Tool: `lgcap`

`package/thingino-lightgun-bench/lgcap.c`. It uses the ISP and FrameSource
only: no encoder, no RTSP, no OSD. Its sensor bring-up follows
`opensensor/openimp` `tools/rawcheck/rawcheck.c`, against the vendor T23 libimp
from SDK 1.3.0, which is the version `ingenic-lib` installs for t23.

Per frame it records:

| Field | Source | Meaning |
|---|---|---|
| `ts` | `IMPFrameInfo.timeStamp` | when libimp/tx-isp completed the frame |
| `get` | `IMP_System_GetTimeStamp()` after `GetFrame` returns | when the app has the frame (same clock as `ts`) |
| `lat` | `get - ts` | ISP-done → app latency. This isn't photon-to-app latency |
| `wait` | time blocked inside `GetFrame` | ≈ frame period when the app keeps up |
| `proc` | time spent on the workload | luma hash only, or threshold + CCL |
| `hash` | FNV over a sparse luma grid | detects repeated (duplicated) frame content |

Summary values:

- **delivered fps**: frames returned ÷ wall time. This is what the app gets.
- **isp fps**: frames ÷ span of `ts`, plus the median `ts` interval. This shows
  the real ISP output cadence, even when the app drops frames.
- **dropped**: `ts` gaps longer than 1.5 × the median, counted in missed periods.
- **duplicate ts / identical content**: a zero or negative `ts` interval, or an
  unchanged content hash. A real scene with sensor noise should never repeat a
  hash, so repeats point to buffer re-delivery.
- **CPU**: whole-system busy % from `/proc/stat`, plus lgcap's own share of one
  core from `/proc/self/stat`.
- **Memory**: lgcap VmRSS/VmHWM, system MemAvailable, and the VB buffer
  footprint (`nrVBs × W × H × 1.5`).
- **LED test** (`-L GPIO`): switches the IR LEDs on (GPIO 50 on this camera)
  and measures the time until a frame whose centre luma has risen reaches the
  app. This is the only true photon→app latency number. It includes exposure,
  readout, ISP and delivery. Point the camera at a nearby surface that reflects
  the LEDs.

**Distinct frames:** "60 fps" only counts when the `ts` intervals sit at
16.7 ms with no duplicate timestamps and no identical-content repeats. A rate
the driver merely accepts doesn't count.

**Workload** (`-w 1`, `-w 2`): a luma threshold followed by run-length
connected-component labelling (8-connected union-find over runs) and blob
centroids. `-w 2` samples every 2nd pixel and row. This stands in for an IR
marker detector, to measure the tracking update rate the CPU can sustain.

## Procedure

```sh
lgcap -n 1800 -f 25            # 1080p @ 25
lgcap -n 1800 -f 30            # 1080p @ 30
lgcap -n 1800 -f 30 -w 1       # with workload
lgcap -n 600 -f 30 -L 50       # LED latency
FPS_LIST="25 30" lgbench       # full matrix for the loaded mode
```

The streamer (`prudynt`) has to be stopped first; `lgbench` does that.

## Test setup **[HW]**

- **Camera:** Sonoff CAM Slim Gen2 at 192.168.1.185, running thingino `ciao+2c3eca5` (2026-08-14).
- **Flash:** 16 MB. The partition table adds up to 16384 kB, so thingino.com's "8 MB" is wrong.
- **Memory:** `mem=41M`, with `rmem=23M` reserved for the ISP and encoder.
- **Firmware isn't reflashed.** The camera's bootloader partition (256 kB) differs from the current build's (320 kB).
- **Sensor driver:** the patched driver, rebuilt against the camera's own ingenic-sdk revision `5e5d7d2`. It's loaded at runtime from the SD card with `rmmod sensor_sc2336_t23; insmod sc2336_mode.ko sensor_mode=N`. Its vermagic matches the stock module, and its undefined symbols are a subset of the stock module's. A reboot restores the stock module.
- **Tools:** `lgcap` and `lgbench` run from the SD card. The camera's `libimp.so` is byte-identical (md5 `8cd60494…`) to the one they were built against.
- **Streamer:** prudynt is stopped for every run. No encoder runs.
- **Exposure:** auto (AE), in an indoor room scene.
- **Run length:** 900 frames per run at 25/30 fps; 1800 at 50/60 fps; 10,000 for the stability runs.

## Results: 1080p (sensor_mode=0) **[HW]**

| Request | ISP fps | Delivered | Median interval | Drops / dup ts / repeated | ISP→app p50 / p99 | CPU sys / lgcap |
|---|---|---|---|---|---|---|
| 25 fps, no work | 24.90 | 24.91 | 40.17 ms | 0 / 0 / 0 | 5.06 / 9.91 ms | 19.7% / 10.0% |
| 30 fps, no work | 29.85 | 29.85 | 33.50 ms | 0 / 0 / 0 | 5.57 / 11.60 ms | 23.0% / 13.3% |
| 25 fps, `-w 1` (full 1920×1080 luma) | — | 23.47 | — | ~6% of frames missed | 5.28 / 9.13 ms | 87.3% / 79.2% |
| 30 fps, `-w 1` (full 1920×1080 luma) | — | **14.83** | — | every other frame missed | 5.65 / 11.60 ms | 59.0% / 49.3% |

What the table shows:

- **Measured rates ran about 0.5% under nominal (24.90 / 29.85 fps) with
  the stock driver. That was a driver bug, not the pixel clock.** The stock
  driver lets auto exposure reach VTS − 4 lines, but the SC2336 needs
  exposure ≤ VTS − 6 and otherwise stretches the frame. Measured at 30 fps: a
  fixed exposure of 1196 lines gave 33.50 ms, 1190 lines gave 33.33 ms. The
  patch caps exposure at VTS − 6. Since then, auto exposure holds exactly
  **25.00 and 30.00 fps** (40.000 / 33.333 ms), and **32.00 fps** works at
  full FOV (VTS 1110).
- **The frames are real.** Not one duplicate timestamp or repeated-content
  frame turned up in any run.
- **The CPU can't process full-resolution frames in real time.** The
  threshold + CCL workload takes **30–31 ms per 1080p luma plane** (p50). The
  CPU reads ISP frame buffers at only about 68 MB/s, which is consistent with
  uncached buffer mappings: every frame size measured gives the same MB/s. See
  [ISP_SCALING.md](ISP_SCALING.md).
- **Earlier drop counts were wrong.** The original `lgcap` measured drops
  against the *median* interval, so a steady every-other-frame miss read as
  "7 drops". From mode 1 onward, drops are counted against the nominal sensor
  period (`processed N of ~M sensor frames`). The 1080p workload rows above
  were recomputed by hand from delivered fps.
- **Memory:** lgcap's RSS is about 4.1 MB. The frame buffers come from rmem,
  not process memory: 2 VBs × 3.0 MB at 1080p. Free RAM during runs was about
  1.8 MB, and kernel 3.10 has no MemAvailable.

**[CALC] vs [HW]:** the calculation predicted 81 MHz ÷ (2250 × 1440) = 25.0 fps
and 81 MHz ÷ (2250 × 1200) = 30.0 fps. The hardware measured 24.90 and 29.85 with the stock exposure margin, and exactly 25.00 and 30.00 after the fix.

## Photon→app latency (LED test) **[HW]**

`lgcap -L 50` switches the 940 nm IR LEDs (GPIO 50, active high) on at a
**random phase** within the frame period. It then times the first delivered
frame whose centre luma rises by more than 12, so the measurement covers
exposure, readout, ISP and delivery.

Setup:

- the IR-cut filter out (night mode)
- `daynightd` paused during the run
- fixed exposure: 600 / 300 / 200 lines, about 16.7 / 6.7 / 4.4 ms
- tracker output scaled to 320×184
- the camera facing nearby objects that reflect the LEDs
- 60 events per rate

| Sensor mode | Period | Mean | p50 | p95 | Min | Max |
|---|---|---|---|---|---|---|
| 0: 1080p @30 | 33.3 ms | 26.5 ms | 25.5 ms | 39.3 ms | 11.8 ms | 42.0 ms |
| 1: 720p @60 | 16.7 ms | **15.3 ms** | 15.7 ms | 22.0 ms | 6.2 ms | 22.3 ms |
| 5: 640×360 @115 | 8.7 ms | **7.6 ms** | 7.3 ms | 11.8 ms | 3.5 ms | 12.2 ms |

- **The mean comes to about 0.9 of a frame period** at every rate. So latency
  is set by when the event falls relative to the next exposure of the centre
  rows, plus about 1–3 ms of ISP + delivery. Doubling the frame rate halves
  the latency.
- **An earlier, phase-locked version measured exactly one period every time**
  (33.3 / 16.6 / 8.6 ms), because the LED always switched right after a frame
  arrived. That's why the random phase was added. Those runs are kept in
  `results/led/mode*_f*.txt` for reference.
- **A tracker adds its own processing time on top:** 0.7–3 ms at the scaled
  sizes (see [ISP_SCALING.md](ISP_SCALING.md)).

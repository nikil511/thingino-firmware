# ISP hardware downscaling (Phase 2)

Status: measured on hardware **[HW]** for sensor modes 0 (1080p) and 1 (720p60).

## What scaling can and can't do

- The ISP scaler resizes the **whole** sensor frame, so the field of view is
  preserved apart from aspect rounding. It reduces DDR writes, the bytes the
  CPU has to scan, and buffer memory.
- It **doesn't** change sensor readout. The sensor still reads out
  HTS × VTS per frame at 81 MHz, so the sensor frame rate and the exposure +
  readout part of latency stay the same.
- It can reduce: memory bandwidth, CPU time per frame for the tracker, and
  possibly the ISP-done → app latency.

## Configurations

On T23, `IMPFSChnAttr.picWidth/picHeight` is the output size, with
`scaler.enable/outwidth/outheight`. That's how prudynt sets it up
(`src/video/IMPFramesource.cpp`).

| Run | Channel setup | lgcap args |
|---|---|---|
| native | ch0 1920×1080, no scaler | `-f 30` |
| ch0 scaled | ch0 scaler → W×H | `-o 960x540`, `-o 640x360`, `-o 320x180` |
| separate ch1 | ch0 1920×1080 + ch1 scaler 640×360, read ch1 | `-c 640x360 -r 1` |
| each, with workload | add `-w 1` | |

No encoder runs in any of these.

**[CALC]** Buffer and bandwidth per channel at 30 fps (NV12, 1.5 B/px):

| Output | Frame | 2 VBs | DDR write @30 fps | @60 fps |
|---|---|---|---|---|
| 1920×1080 | 3.0 MB | 6.1 MB | 93 MB/s | n/a |
| 960×540 | 0.78 MB | 1.5 MB | 23 MB/s | 47 MB/s |
| 640×360 | 0.35 MB | 0.69 MB | 10 MB/s | 21 MB/s |
| 320×180 | 86 kB | 173 kB | 2.6 MB/s | 5.2 MB/s |

The luma plane the tracker scans is 2/3 of each frame size.

## libimp size constraint **[HW]**

`IMP_FrameSource_CreateChn` rejects 960×540 and 320×180. libimp logs:

```
check_framesource_chn_attr: picWidth=320 should be align to 16 and picHeight=180 should align to 8
```

So the output width must be a multiple of 16 and the height a multiple of 8.
The test sizes are 960×544, 640×360 and 320×184, all within 2% of 16:9. The
same rule applies to native sensor heights: the experimental 540-line modes
need the scaler to output 536 lines.

## Results: 1080p sensor (mode 0) **[HW]**

`-w 1` is threshold + CCL over the whole output luma plane.

| Output | Channel | Rate | Delivered | ISP→app p50 (no work / work) | Workload p50 | CPU sys / lgcap (work) | Drops |
|---|---|---|---|---|---|---|---|
| 1920×1080 | ch0 native | 30 | 29.85 | 5.57 ms / 5.65 ms | **30.8 ms** | 59.0% / 49.3% | every other frame |
| 960×544 | ch0 scaled | 30 | 29.86 / 29.84 | 1.21 / 2.61 ms | 9.46 ms | 41.5% / 32.1% | 0 |
| 640×360 | ch0 scaled | 30 | 29.85 / 29.85 | 0.75 / 2.12 ms | 3.42 ms | 22.2% / 12.5% | 0 |
| 320×184 | ch0 scaled | 30 | 29.88 / 29.85 | 0.45 / 0.44 ms | 1.07 ms | 13.8% / 4.6% | 0 |
| 640×360 | ch1 (+ ch0 1920×1080) | 30 | 29.85 | — / 2.36 ms | 3.35 ms | 22.4% / 13.0% | 0 |
| 640×360, 10,000 frames | ch0 scaled | 30 | 29.85 | 2.12 ms (p99 2.84, max 16.0) | 3.37 ms (p99 4.39) | 21.9% / 12.4% | **0** |

At 25 fps the numbers are the same within noise.

## Results: 720p60 sensor (mode 1) **[HW]**

| Output | Channel | Rate | Delivered | ISP→app p50 (no work / work) | Workload p50 / p99 | CPU sys / lgcap (work) | Sensor frames processed |
|---|---|---|---|---|---|---|---|
| 1280×720 | ch0 native | 60 | 60.00 / **47.10** | 3.51 / 3.62 ms | 12.5 / 28.9 ms | 84.4% / 69.6% | 78.5% |
| 960×544 | ch0 scaled | 60 | 60.01 / 58.59 | 1.22 / 2.83 ms | 7.19 / 17.1 ms | 64.4% / 50.3% | 97.7% |
| 640×360 | ch0 scaled | 60 | 60.01 / 59.96 | 0.81 / 2.28 ms | 3.03 / 3.72 ms | 31.0% / 23.3% | 99.9% |
| 320×184 | ch0 scaled | 60 | 60.01 / 60.00 | 0.48 / 0.48 ms | 0.77 / 0.96 ms | 24.1% / 7.6% | **100%** |
| 640×360 | ch1 (+ ch0 1280×720) | 60 | 59.89 | — / 2.52 ms | 3.03 / 3.94 ms | 42.4% / 24.7% | 99.8% |
| 640×360, 10,000 frames | ch0 scaled | 60 | 59.98 | 2.28 ms (p99 3.09, max 14.1) | 3.03 / 3.89 ms | 38.2% / 23.5% | 10,000 of ~10,003 |

## Findings

1. **Scaling preserves the FOV.** The scaled frames show the same scene
   extent as native. It's the whole frame resized, not a crop.
2. **Scaling is what makes real-time tracking possible.** CPU reads of ISP
   buffers run at a constant **~68 MB/s** (1080p: 2.07 MB in 30.8 ms; 640×360:
   230 kB in 3.4 ms; 320×184: 59 kB in 0.77–1.07 ms). That rate is consistent
   with uncached memory. At full resolution, neither 1080p30 nor 720p60 can be
   processed in real time. At 640×360 both can, with 3 ms per frame.
3. **Scaling cuts delivery latency.** ISP-done → app falls from 5.6 ms (1080p)
   or 3.5 ms (720p) to 0.45–0.8 ms at small outputs. The time the ISP needs to
   finish writing the output buffer evidently scales with its size.
4. **The workload itself adds about 1.4 ms of latency** at 640×360 (0.8 →
   2.3 ms p50), but not at 320×184. The cause is unexplained; possibly memory-bus
   contention while the CPU scans uncached memory as the ISP writes the next
   frame.
5. **A separate ch1 scaler costs about 0.25 ms more latency than scaling ch0**,
   and a bit more system CPU (the second DMA stream). It's useful if a
   full-resolution stream is also wanted, for example for RTSP preview.
6. **The sensor rate is unchanged by scaling**, as expected: 25/30/60 fps in
   every configuration.

To check that FOV is kept, compare `fov_<size>_f<fps>.pgm` with
`fov_native_f<fps>.pgm` from the same `lgbench` run (see
[FOV_COMPARISON.md](FOV_COMPARISON.md)).

# Recommendation

Status: based on hardware measurements **[HW]** on a Sonoff CAM Slim Gen2
(T23N + SC2336), except where marked **[CALC]**. The lens angle isn't measured
yet, which affects only the FOV judgement.

## Use now: mode 1 (720p60), with the ISP scaling to 640×360 or 320×184

| | Result |
|---|---|
| Sensor / delivered rate | 60.00 fps, median interval 16.667 ms, no duplicate or repeated frames |
| 10,000-frame tracking run (640×360, threshold + CCL) | 10,000 of ~10,003 frames processed, workload p50 3.0 ms, 23.5% of a core |
| Same at 320×184 | 100% processed, workload 0.77 ms, 7.6% of a core |
| Photon → app latency (IR LED, random phase) | mean **15.3 ms**, p95 22.0 ms, max 22.3 ms |
| FOV | centre 2/3 × 2/3 of the full sensor view |
| Risk | lowest: a published register table, the stock PLL and MIPI rate |

Why this mode:

- It's the fastest mode with a vendor-published register table.
- It keeps a balanced 2/3 × 2/3 view. **[CALC]** With a ~100° lens that's
  about 77 × 48°, enough for four corner markers on a 55–65" TV at 2–3 m.
- 640×360 gives the tracker roughly 8 px per degree. 320×184 halves CPU and
  latency but also halves marker precision.

## Alternatives the hardware supports

| Option | Rate | FOV (H × V of full) | Latency (mean) | When to prefer it |
|---|---|---|---|---|
| **Mode 2:** 1920×540 strip → ISP 640×184 | 60 (max 63) | **1 × 0.5** | ≈15 ms (same period as mode 1) | The player moves sideways a lot and the TV height fits in half the vertical view. 0 drops in 10,000 frames, workload 2.1 ms |
| **Mode 5:** 640×360 crop → ISP 320×184 | **110–115** | 1/3 × 1/3 | **7.6 ms** | Only with a wider lens (below). 10,000 frames at 110 fps, 1 drop, 12.5% CPU |
| Mode 4: 640×480 crop | 88 | 1/3 × 0.44 | ≈10 ms **[CALC]** | taller aspect, otherwise like mode 5 |
| Mode 3: 960×540 crop | 78 | 1/2 × 1/2 | ≈11 ms **[CALC]** | middle ground |
| Mode 6: 1920×360 strip → 640×120 | 92 | 1 × 1/3 | ≈10 ms **[CALC]** | very wide, very short view |
| Mode 0: 1080p full FOV → ISP 640×360 | 30–**32** | 1 × 1 | 26.5 ms | only if FOV matters more than rate |

Not viable today:

- **Full FOV above 32 fps.** The SC200AI binning recipe (mode 7) gives a
  corrupted image.
- **120 fps.** The T23N ISP needs at least 30 lines of vertical blanking.

## Credible path past 60 fps: mode 5 plus a wider lens

**[CALC]** Behind a ~150° M12 lens, the 640×360 crop would see about
2·atan(tan 75° / 3) ≈ 102° horizontally and about 70° vertically. That's
*more* than mode 1 gives with the stock lens, at **110–115 fps and roughly
half the latency**. The costs:

- fisheye distortion, which the tracker must undistort
- fewer pixels per degree (≈6 vs ≈8)
- an unknown lens mount, which needs opening the camera to check

This is the experiment to run before giving up on >60 fps.

## Required regardless of mode

- **Always track on an ISP-scaled channel.** The CPU reads ISP frame buffers at
  about 68 MB/s, so full-resolution frames can't be processed in real time
  even at 25 fps. Scaling also cuts ISP→app latency to under 1 ms.
- **Keep scaled sizes on libimp's grid:** width a multiple of 16, height a
  multiple of 8.
- **Use fixed exposure for IR tracking.** Short exposures (≤ 4–7 ms) keep
  markers sharp and stop auto exposure from changing brightness mid-game.
- **Take the driver's exposure fix (VTS − 6) with any mode.** The stock VTS − 4
  stretches frames in dim scenes.

## Next steps, in order

1. Measure the lens FOV (tape grid at a known distance), then re-check mode 1
   and mode 2 coverage for the target TV size and distance.
2. Decide between mode 1 and mode 2. Then build the IR-marker detector on the
   scaled channel, reusing `lgcap`'s capture loop and CCL.
3. In parallel, try a wide M12 lens with mode 5 (110 fps).
4. Then UDP coordinates and the ESP32-S3 HID, as originally planned.

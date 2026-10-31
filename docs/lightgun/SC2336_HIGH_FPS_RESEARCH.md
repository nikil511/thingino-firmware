# SC2336 above 60 fps (Phase 4)

Status: research **[SRC]/[CALC]**, then every mode measured on hardware
**[HW]**. The real limit turned out to be the T23N ISP's minimum vertical
blanking, not the sensor clock. Full source notes, with file/line references and
complete register tables, are in [SC2336_SOURCES.md](SC2336_SOURCES.md).

## Findings from source **[SRC]**

- **Sources checked:**
  - all 11 Ingenic SDK SC2336 drivers (t23/t31/t40/t41)
  - all 14 Espressif `esp_cam_sensor` SC2336 modes
  - the Rockchip, SigmaStar, Sophgo and Goke drivers
  - Sipeed BL808, whose header is empty, and Adafruit, whose modes are copies of Espressif's
- **Fastest published modes:** 1080p at 30 fps, 720p at 60 fps, 640×480 at
  50 fps. Nobody publicly reports an SC2336 above 60 fps.
- **Every mode smaller than 1080p is a centre crop** (window registers
  0x3200–0x3207). No SC2336 table writes the SmartSens skip/bin registers
  (0x3215, 0x3220, 0x5000, 0x5900/0x5901).
- **Highest pixel clock in any table:** 84 MHz, and only as RAW8. The stock
  T23 driver and the 720p60 table use 81 MHz (405 Mbps × 2 lanes ÷ 10 bit). The
  PLL register formula isn't public.
- **The T23 isn't the bottleneck.** T23 drivers in the same SDK run 2-lane MIPI
  at 720–1080 Mbps/lane, and `sc3336p` pushes 2304×1296@45 (134 Mpix/s) through
  the T23 ISP at the default 200 MHz `isp_clk`. The SC2336 tops out at about
  81–84 Mpix/s.

## Which mechanisms can raise sensor fps

| Mechanism | Changes sensor fps? | FOV | Status on SC2336 |
|---|---|---|---|
| ISP downscale | no | kept | supported by the ISP; see [ISP_SCALING.md](ISP_SCALING.md) |
| Vertical window (fewer rows → smaller VTS) | **yes**, roughly in proportion | V reduced | proven: 720p60 |
| Horizontal window (shorter HTS) | yes, only down to the minimum HTS | H reduced | HTS 1800 proven at 1280 wide; the minimum is unknown |
| Skip / bin | yes | **kept** | unpublished for SC2336; mode 7 tries the SC200AI recipe |
| Higher pixel clock | yes | kept | +3.7% (84 MHz) is the most any table shows; not attempted |

## Timing limits **[CALC]**

`fps = 81 MHz / (HTS × VTS)`. Minimum VTS = active rows + blanking; published
blanking is 30–45 lines, and the driver allows down to 8. Minimum HTS for
widths of 1280 or less is assumed to be 1800, the smallest published value.

| Native window | HTS | VTS for target | fps | Notes |
|---|---|---|---|---|
| 1920×1080 | 2250 | 1125 | 32.0 | ceiling at full FOV without binning; 40/45 fps impossible at 81 MHz |
| 1280×720 | 1800 | 750 / 600 / 500 | 60 / 75 / 90 | 75 and 90 need VTS under 728 → impossible at this HTS |
| 960×540 | 1800 | 600 / 570 | 75 / 79 | 90 needs VTS 500 < 548 → impossible |
| 800×600 | 1800 | 630 | 71 | 90 impossible |
| 640×480 | 1800 | 600 / 500 | 75 / 90 | 120 needs VTS 375 < 488 → impossible |
| 640×360 | 1800 | 500 / 375 | 90 / 120 | 120 leaves only 7 blanking lines |
| 1920×540 strip | 2250 | 600 | 60 | full horizontal FOV |
| 1920×360 strip | 2250 | 400 | 90 | full horizontal FOV |
| 960×540 bin 2×2 | 2250 | 600 | 60 | full FOV, if binning works |

The only ways past these numbers would be a narrower HTS (unknown minimum) or
a faster PLL (unknown formula). Neither is attempted here: no blind PLL
overclocking.

## Experimental driver modes

Every mode keeps the stock 81 MHz PLL and 405 Mbps MIPI, so the T23 CSI
settings don't change. Each derived mode works like this:

1. Write a published base table (720p60 for HTS 1800 crops, stock 1080p for
   full-width strips and binning), skipping its stream-on.
2. Reprogram the window, output size, HTS, VTS and initial exposure.
3. Stream on.

Window geometry follows the published crops: an array window of (W+32) ×
(H+8), centred in 1928 × 1088, with output offset (16, 4). The measured FOV
overlay confirms these placements to within 10 px
([FOV_COMPARISON.md](FOV_COMPARISON.md)).

## Results **[HW]**

Each row is 600 frames (3,300–10,000 for the workload/stability rows), auto
exposure, native output unless noted. 540-line modes go through the scaler to
536 lines, because libimp needs heights that are multiples of 8.

| `sensor_mode` | Output | Type | Clean (zero drops, exact period) | Fails at | Blank lines (VTS − rows) OK / fail | Image |
|---|---|---|---|---|---|---|
| 0 | 1920×1080 | full FOV | 25, 30, **32.00** | — (not tried above 32) | 30 at 32 fps | clean |
| 1 | 1280×720 | published crop | 50, **60.00** | — | 30 | clean |
| 2 | 1920×540 | full-width strip | 60, **63.05** | 65 → 17 fps, noise | 31 / 13 | clean ([img](img/mode2_1920x540_strip.png)) |
| 3 | 960×540 | crop | 75, **78.12** | 80 → no frames | 36 / 22 | clean |
| 4 | 640×480 | crop | 75, 80, 85, **88.06** | 90 → no frames | 31 / 20 | clean |
| 5 | 640×360 | crop | 90, 100, 110, **115.09** | 120 → no frames, noise | 31 / 15 | clean ([img](img/mode5_640x360_115fps.png)) |
| 6 | 1920×360 | full-width strip | 90, **92.07** | 95 → no frames | 31 / 18 | clean |
| 7 | 960×540 | 2×2 bin/skip (SC200AI regs) | 30 (period OK) | 60 → **49.3 fps** | — | **corrupt** ([img](img/mode7_bin_corrupt.png)) |

Highest measured rates with the tracking workload:

| Mode, rate | Tracker input | Frames | Processed | Workload p50 / p99 | lgcap CPU |
|---|---|---|---|---|---|
| 5 @110 | ISP → 320×184 | 10,000 | 10,000 of ~10,001 | 0.66 / 1.56 ms | 12.5% |
| 5 @110 | native 640×360 | 3,300 | 99.4% | 2.43 / 3.18 ms | 35.6% |
| 6 @90 | ISP → 640×120 | 2,700 | 100% | 0.86 / 1.09 ms | 12.4% |
| 2 @60 | ISP → 640×184 | 10,000 | **100%** (0 drops) | 2.10 / 3.11 ms | 11.4% |
| 1 @60 | ISP → 640×360 | 10,000 | 10,000 of ~10,003 | 3.03 / 3.89 ms | 23.5% |

## What limits the frame rate **[HW]**

1. **The T23N ISP needs at least 23–31 lines of vertical blanking.** Every
   mode works with ≥30 blank lines and fails with 13–22, whatever the HTS
   (1800 or 2250), resolution or orientation. With too little blanking the ISP
   either delivers nothing (`GetFrame` times out) or delivers noise frames at
   a reduced rate. The sensor isn't the limit: at 81 MHz it would read these
   windows faster. The driver now enforces `min_vts = rows + 30`, and every
   mode's `max_fps` is derived from that, so a default request is always a
   working one.
2. **The SC2336 has a frame-length rule the stock driver violates.** Exposure
   must be ≤ VTS − 6. The stock T23 driver allows VTS − 4, so in a dim scene
   (auto exposure at its maximum) the sensor stretches every frame. 1080p30
   measured 33.50 ms instead of 33.33: a fixed exposure of 1196 lines gave
   29.85 fps, 1190 lines gave 30.00. Fixed in the patch (margin 6 for every
   mode); afterwards auto exposure holds exactly 25.00 / 30.00.
3. **HTS 1800 works for every width up to 1280; 2250 is needed at 1920.**
   Narrower HTS values for 640-wide windows weren't tried, to avoid blind
   timing changes. That's the next lever (see below).
4. **Binning or skipping (mode 7):** the SC200AI recipe makes the SC2336 read
   a wider area (the frame shows more of the scene). But the output has
   zig-zag comb artifacts and banding, and the frame stretches to about 730
   lines (49 fps at a 600-line VTS). The sensor does have some
   subsampling/skip logic; the SC200AI values just don't drive it correctly.
   A full-FOV mode above 32 fps stays an open research item.

## Answers to the original questions

- **Genuine 60 fps:** yes. Mode 1 (published) and mode 2 (strip) both hold
  60.00 fps over 10,000 frames with no duplicates.
- **ISP downscaling without losing FOV:** yes. It keeps the FOV, and it's
  required: the CPU reads frame buffers at about 68 MB/s, so only scaled
  outputs can be processed at 60+ fps.
- **Undocumented higher-FPS modes:** yes, as crops. 75 (960×540), 88
  (640×480), 92 (1920×360) and **115 fps (640×360)** all run clean.
  1280×720 above 60 isn't possible at HTS 1800 with 30 blank lines
  (45000 ÷ 750 = 60). 120 fps needs VTS 375 < 390.
- **Full FOV:** 1080p maxes out at 32 fps (VTS 1110). 40 and 45 fps at full
  FOV would need working binning or a faster pixel clock. Neither is
  available today.

## Next levers (not tried)

- **Narrower HTS for the 640-wide crops.** At HTS 1400, mode 5 would reach
  57857 ÷ 390 = 148 fps. Try it in steps of 100, checking the image for ADC
  artifacts.
- **Find the right SC2336 skip registers.** Diff 0x3215/0x3220 bit-by-bit,
  starting with 0x3215 alone (row skip only) and no digital bin.
- **The 84 MHz RAW8 PLL** from the ESP 1080p RAW8 table: +3.7% at most.

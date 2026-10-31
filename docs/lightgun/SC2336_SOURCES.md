# SC2336 on Ingenic T23N (Sonoff CAM Slim Gen2): how fast can it read out?

Date: 2026-10-30. Scope: read-only research. Paths below are local copies unless a URL is given.
Abbreviations: ESP = espressif/esp-video-components @ 08bcabdf (2026-10-28), path `esp_cam_sensor/sensors/sc2336/`.
Local ESP copy: `scratchpad/dl-esp/esp-video-components/...`. Ingenic SDK drivers: `scratchpad/sdk-sc2336/`.
T23 SDK drivers for other sensors: themactep/ingenic-sdk @ 9bb8ad51, `common/sensor/t23/` (local: `scratchpad/dl-isdk/`).

## TL;DR

* **1080p full field of view: about 32-33 fps is the ceiling at the published 81 MHz pixel clock** (HTS 2200-2250, VTS ≥ 1125). No published SC2336 table goes above 30 fps at 1080p, and none uses a pixel clock above 84 MHz.
* **720p (centre crop) at 60 fps is published** (ESP). It uses the **same PLL and 405 Mbps/lane as the stock T23 driver**. Only the window, HTS 1800, VTS 750, 0x3395 and 0x3641 change, so it is the safest port.
* **No SC2336 binning or skipping mode exists in any source found.** Every sub-1080p table (ESP, Ingenic `sc2336p`) is a centre crop that narrows the FOV. The SmartSens skip and bin registers (0x3215/0x3220/0x5000/0x5900/0x5901) are not written by any SC2336 table.
* Faster small modes come only from **vertical crop (smaller VTS) at HTS 1800**. A 640x480 crop could reach about 88 fps and a 640x360 crop about 115 fps at 81 MHz. These are inferred and untested; 60 fps is the highest rate actually published.
* T23 side: vendor T23 drivers in the same SDK run 2-lane MIPI at 720-1080 Mbps/lane. `sc3336p` (H20250206a) runs 2304x1296@45 = 134 Mpix/s active through the T23 ISP. **The T23 is not the bottleneck at the SC2336's 81-84 Mpix/s.** The sensor is.

---

## (a) Facts read from source

### A1. Ingenic SDK SC2336 variants (local `scratchpad/sdk-sc2336/`)

Two different silicon families are covered:
* `sc2336`, `sc2336s0`, `sc2336s1`: chip ID **0xCB3A**. For example `t23_sc2336.c:23-24`.
* `sc2336p`, `sc2336ps0`, `sc2336ps1`: chip ID **0x9B3A**. For example `t23_sc2336p.c:28-29`.

The Sonoff CAM Slim Gen2 config uses `BR2_SENSOR_1_NAME="sc2336"` (`configs/cameras/sonoff_s2_t23n_sc2336_atbm6012bx/..._defconfig`), which is the 0xCB3A part. The ESP driver also probes PID 0xCB3A (`include/sc2336.h:16`), so **ESP tables target the same silicon**. The `p` tables use different PLL values and should not be mixed in.

| file | chip | MIPI bus (.clk, lanes) | HTS | VTS | fps | 0x301f | PLL 36ea-ed / 37fa-fd, final 36e9/37f9 | other modes |
|---|---|---|---|---|---|---|---|---|
| t23_sc2336.c (H20210805a) | CB3A | 405, 2 (:277-278) | 0x8ca=2250 (:329-330) | 0x5a0=1440 (:331-332) | win 25 (:479), MAX_FPS 30 (:29), SCLK 81 MHz (:33) | 02 | 09 0c 1c 28 / 09 32 11 37, 53/53 | none |
| t31_sc2336.c | CB3A | 405, 2 | 2250 | 1440 | 25 | 02 | same as t23 | none |
| t41_sc2336.c (H20231116a) | CB3A | 405, 2 | 2250 (:337, comment) | 0x5a0 (:339, comment says "1200", value is 1440) | 25, MAX 25 | 98 | same | none |
| t23_sc2336s0/s1.c | CB3A | 405, 2 | 2250 | 0x963/0x95d (~2400) | 15, MAX 15 | 98 | same | none |
| t23_sc2336p.c / t31_sc2336p.c (H20231223a) | 9B3A | 396, 2 | default (0x898=2200 per t40 comment :476) | 0x4b0=1200 / 0x960=2400 | 15, 30 | 09 | 0b 0c 1c 18 / cb 32 11 07, 53/33 | **640x360 crop @15 fps** (:546-720) |
| t23_sc2336ps0/ps1.c | 9B3A | 396, 2 | default | ~2400 | 15 | 1b | p-PLL | none |
| t40_sc2336p/ps1.c | 9B3A | 396, 2 | default | 0x708=1800 | 20 | 09 | p-PLL | none |

* The register body of `t23_sc2336.c` is identical in structure to ESP `1920x1080_raw10_30fps.h`. Both carry the "cleaned 0x02 ... 405Mbps 10bit 1080p@30fps" header, and both have **identical PLL and MIPI timing registers (0x4819-0x4829)**. The only diffs are VTS (1440 vs 1200), the exposure defaults and analog trims 0x330d, 0x3674, 0x3690-0x3692, 0x3699-0x369b, 0x3934, 0x3940-0x3943 (diff run locally).
* Ingenic `sc2336p` 640x360 mode: window x 0x274..0x513 (628..1299), y 0x168..0x2d7 (360..727), output 640x360, offsets 0x10/0x04. That is a **centre crop**. VTS stays 2400 (15 fps), so it gives no speed gain. Its `sensor_mipi_1` says 640x**320** (`t23_sc2336p.c:285-286`), which looks like a typo.
* Ingenic `set_fps` (`t23_sc2336.c:741-779`): `vts = 81e6 * den / hts / num`. It rejects fps > `SENSOR_OUTPUT_MAX_FPS`. It sets `max_integration_time = vts - 4`.

### A2. Espressif esp_cam_sensor SC2336: every mode header (`private_include/`)

"Comment" is line 6 of each header. pclk = HTS × VTS × fps, which also equals lane_rate × lanes / bpp in every row. Window = 0x3200-0x3207, out = 0x3208-0x320b, offset = 0x3210-0x3213.

| header | Mbps/lane × lanes | fmt | HTS | VTS | fps | pclk | 0x301f | 36ea-ed | 37fa-fd | 36e9/37f9 final | window x / y | out | FOV |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mipi_2lane_1920x1080_raw10_30fps | 405×2 | RAW10 | 2250 | 1200 | 30 | 81.0 M | 02 | 09 0c 1c 28 | 09 32 11 37 | 53/53 | default (full) | 1920x1080 | full |
| mipi_2lane_1920x1080_raw10_25fps | 330×2 | RAW10 | default (2200) | 1200 | 25 | 66.0 M | 28 | not written | not written | 20/27 | full | 1920x1080 | full |
| mipi_2lane_1920x1080_raw10_15fps | 405×2 | RAW10 | 2250 | 2400 | 15 | 81.0 M | 02 | 09 0c 1c 28 | 09 32 11 37 | 53/53 | full | 1920x1080 | full |
| mipi_1lane_1920x1080_raw10_25fps | 660×1 | RAW10 | default (2200) | 1200 | 25 | 66.0 M | 29 (+0x3018=0x12, 0x3019=0x0e) | 36ec=0c only | not written | 20/27 | full | 1920x1080 | full |
| mipi_2lane_1920x1080_raw8_30fps | 336×2 | RAW8 | 2240 | 1250 | 30 | 84.0 M | 8e (+3031=08, 3037=00) | 0e 0a 1a 18 | 15 32 11 17 | 54/54 | full | 1920x1080 | full |
| mipi_2lane_1280x720_raw10_25fps | 405×2 | RAW10 | 1800 | 1800 | 25 | 81.0 M | 2d | 09 0c 1c 28 | 09 32 11 37 | 53/53 | 308..1619 / 180..907 | 1280x720 | crop |
| mipi_2lane_1280x720_raw10_30fps | 405×2 | RAW10 | 1800 | 1500 | 30 | 81.0 M | 2e | same | same | 53/53 | same | 1280x720 | crop |
| mipi_2lane_1280x720_raw10_50fps | 405×2 | RAW10 | 1800 | 900 | 50 | 81.0 M | 2f | same | same | 53/53 | same | 1280x720 | crop |
| **mipi_2lane_1280x720_raw10_60fps** | **405×2** | RAW10 | **1800** | **750** | **60** | **81.0 M** | 30 | same | same | 53/53 | same | 1280x720 | **crop** |
| mipi_2lane_1280x720_raw8_30fps | 336×2 | RAW8 | 2240 | 1250 | 30 | 84.0 M | 8e | 0e 0a 1a 18 | 15 32 11 17 | 54/54 | same | 1280x720 | crop |
| mipi_2lane_1024x600_raw8_30fps | 288×2 | RAW8 | 2400 | 1000 | 30 | 72.0 M | c7 | 08 0a 1a 18 | 08 32 11 37 | 53/53 | 436..1491 / 240..847 | 1024x600 | crop |
| mipi_2lane_800x800_raw10_30fps | 336×2 | RAW10 | 2240 | 1000 | 30 | 67.2 M | a7 (+3031=0a, 3037=20) | 0e 0c 1c 18 | 15 32 11 17 | 54/30 | 548..1379 / 140..947 | 800x800 | crop |
| mipi_2lane_800x800_raw8_30fps | 336×2 | RAW8 | 2240 | 1250 | 30 | 84.0 M | a6 | 0e 0a 1a 18 | 15 32 11 17 | 54/54 | same | 800x800 | crop |
| **mipi_2lane_640x480_raw10_50fps** | 210×2 | RAW10 | 1600 | 525 | 50 | 42.0 M | a8 | 07 1c 1c 28 | 07 32 11 37 | 24/20 | 628..1299 / 300..787 | 640x480 | **crop** |
| dvp_8bit_1280x720_raw10_30fps | DVP | RAW10 | default | 750 | 30 | 49.5 M (name) | 2a | 0b 0c 1c 28 | cb 32 11 07 | 51/53 | 320..1607 / 180..907 | 1280x720 | crop |

Notes from the ESP source:
* **Binning and skipping: none.** No ESP SC2336 table writes 0x3215, 0x3220, 0x5000, 0x5900 or 0x5901. All non-1080p modes are centre crops via 0x3200-0x3207, with output size in 0x3208-0x320b.
* 720p60 vs 1080p30 diff (ESP, computed locally): 0x301f (02→30), window and HTS/VTS registers, **0x3395 0xff→0x30**, **added 0x3641=0x03** ("PAD driving"). PLL and MIPI timing are unchanged.
* 640x480@50 vs 720p60: a different, lower PLL (210 Mbps). It adds 0x3250=0x40 and 0x4800=0x44, and changes 0x3395=0x20, 0x33f9=0x78, 0x34ab=0x28, 0x3690-92=22 22 22 and the MIPI timing 0x4819-0x4829. It drops 0x3641.
* `sc2336.c:60-61`: `SC2336_VTS_MAX 0x7fff`, `SC2336_EXP_MAX_OFFSET 6` (exposure ≤ VTS-6). `sc2336.c:1505-1511`: VTS may only be increased above a mode's default.
* The ESP `isp_info` metadata table is partly inconsistent with the register tables. For example, entry [3] (1-lane 1080p25) says pclk 81 MHz and HTS 1125, but tline is 33.3 µs. Entry [7] (640x480) carries 800x800 values. **Trust the register tables, not `isp_info`.**
* 1-lane 660 Mbps uses the same final PLL bytes (36e9=0x20, 37f9=0x27) as 2-lane 330 Mbps. Both run at 66 MHz pclk. So 660 Mbps is **the same pixel rate serialized onto one lane**, not a faster sensor clock.

### A3. Other SC2336 drivers

| source | modes | MIPI | HTS/VTS |
|---|---|---|---|
| Rockchip `drivers/media/i2c/sc2336.c` (unifreq/linux-6.1.y-rockchip; identical copies in ArmSoM rk3506, TaishanPi, ANYCUBIC Kobra3) | 1920x1080@30 only | LINK_FREQ 202.5 MHz = 405 Mbps, 2-lane (:37-42) | 0x8ca / 0x4b0 (:340-341) |
| SigmaStar infinity6b0 (OpenIPC/sensors `sigmastar/infinity6b0/sensor_sc2336_mipi.c`) | 1920x1080@30 only | **27 MHz MCLK**, default PLL (36e9=0x20, 37f9=0x27) | VTS 0x465=1125 (:315, :525), line 29.63 µs (:60) → 74.25 Mpix/s with default HTS 2200 |
| Sophgo cvi_alios_open `sms_sc2336` | 1080P30 only (`SC2336_MODE_1080P30`) | | |
| Goke gk7205v500 (OpenIPC/firmware `sc2336_i2c_1080p.ini`) | 1080p, Isp_FrameRate=25 | | |
| Sipeed M1s_BL808_SDK `components/stage/sensor/include/sc2336.h` | **empty header** (4 lines, no table) | | |
| Adafruit_CircuitPython_SC2336 `modes.py` | 800x800r8@30, 640x480@50, 720p@30/50, 1080p@30: copies of ESP tables | | |
| ESoPe-GmbH/sld_demo `app_sc2336_custom_settings.h` | commented-out copy of ESP 800x800 RAW8 | | |

* **No public SC2336 datasheet was found.** The only public spec text (GoPhotonics listing, via search snippet; the page returned 403 to direct fetch) says 1928x1088, 2.7 µm pixels, 1/3", **up to 30 fps at 1920x1080**, 10-bit DVP or 8/10-bit 1/2-lane MIPI. No binning or skip feature is advertised.
* OpenIPC/firmware#1324 ("~41 fps") is about the **SC223A** on a Goke SoC, not the SC2336. It is irrelevant here.

### A4. T23/T23N CSI and ISP limits

* thingino `Config.soc.in:190`: the default `isp_clk` for the t23 family is **200 MHz**. Selectable values run from 90 to 350 MHz (`thingino.mk` resolve_clock_freq for ISP_CLK).
* Ingenic T23 sensor drivers in the SDK (`common/sensor/t23/`, themactep/ingenic-sdk @9bb8ad51) with MIPI rates and frame rates above SC2336 stock:
  * `sc3336p.c` (H20250206a, T23-era): 2304x1296@**45**, 2-lane **743 Mbps** (:299-300), HTS 1250 × VTS 1320 × 45 (:351-357, :971-978). That is **134 Mpix/s active**, about 149 Mpix/s on the wire.
  * `cv2003s0.c` (H20240219a): 2-lane **960 Mbps**, 1080p30. `os02n10s0.c` (H20240219a): 783 Mbps. `sc1346s0.c` and `sc1a4t.c`: 720 Mbps.
  * `sc301iot.c` (H20230628a): 2-lane **1080 Mbps** (WDR DOL struct, :353-356).
  * `gc1084s0.c` (H20241226a): 1280x720@**50**, 1-lane 648 Mbps (:150-160).
  * Older drivers with 2019/2020 version strings, probably carried over from T31: `os02k10.c` (640x480@120 RAW12, 1080p@60, 768 Mbps), `os02d20.c` (640x480@120, 1080p@60), `imx327.c` (1080p@60, 891 Mbps), `os04c10.c` (1080p@60), `sc200ai.c` (1080p@60), `ov5648.c` (720p@60).
* `tx-isp-common.h:302-318` enumerates `TX_SENSOR_MAX_FPS_120 ... _5`.
* No official Ingenic T23N datasheet was found. A secondary CSDN post about the **T23ZN** (not the T23N) claims "ISP 3MP@30fps" and "MIPI-CSI 2lane 1.5Gbps". It is unverified.

### A5. SmartSens binning convention, from related sensors (not SC2336)

* Rockchip `sc200ai.c` (develop-5.10) has `sc200ai_linear_10_960x540_120fps_regs` (:672). Key writes: 0x3215=0x31, 0x3220=0x17, 0x5000=0x46, 0x5900=0xf1, 0x5901=0x04, out 960x540, VTS 0x232.
* Rockchip `sc450ai.c` ":17 add 4lane 120fps 1344x760 binning setting" (:812-1168). Key writes: 0x3215=0x31, 0x3220=0x01, 0x5000=0x4e, 0x5900=0x01, 0x5901=0x04.
* These are different SmartSens die. Whether the SC2336 implements the same registers is **unknown**.

---

## (b) Inferences (reasoned, not verified on hardware)

1. **Pixel-clock model.** For every SC2336 table, fps = pclk / (HTS × VTS) with pclk = lane_Mbps × lanes / bits. So HTS is in pclk units, and the sensor emits one pixel per pclk. 81 MHz is the T23 stock value. **84 MHz is the highest pixel clock in any published SC2336 table** (ESP RAW8, 336 Mbps). Running 84 MHz as RAW10 would need 420 Mbps/lane, but the ESP 84 MHz PLL bytes are tied to RAW8 MIPI settings, so no ready-made RAW10 84 MHz table exists. Nothing supports a pixel clock above 84 MHz, and the vendor spec caps 1080p at 30 fps.
2. **HTS floor.** 1920-wide modes use HTS 2200-2250 (min seen 2200). 1280-wide crops use **HTS 1800 at 81 MHz**, a 22.2 µs line, which is the shortest line time in any published table. The 640-wide ESP mode uses HTS 1600, but at 42 MHz (a 38 µs line), so it says nothing about the floor. The true minimum HTS for narrow crops is **unknown**. HTS 1600 and 1400 below are speculative.
3. **VTS floor.** Published blanking above active height: 30 lines (720p60, VTS 750), 45 lines (1080p with VTS 1125 in SigmaStar; 640x480 with VTS 525), 120 lines (1080p, VTS 1200). The tables below assume active + 30 and active + 45. Exposure is limited to VTS-6 lines (ESP) or VTS-4 (Ingenic).
4. **What actually speeds up readout on this sensor:**
   * **Vertical crop (fewer rows, smaller VTS):** yes. This is the only proven mechanism (720p60).
   * **Horizontal crop:** it allowed HTS 2250 → 1800 at 1280 wide. Whether HTS can drop further for narrower windows is unknown.
   * **ISP scaling on the T23:** gives zero sensor speed-up. The sensor still reads the full window.
   * **Skip/bin:** none published for SC2336. It would be the only way to keep the full FOV above about 33 fps. Porting the SC200AI/SC450AI recipe is speculative and could misbehave.
   * **Clock increase:** at most +3.7% (81 → 84 MHz) is evidenced. Anything beyond that is unverified overclocking of the sensor PLL. The PLL register formula is unknown (see (c)).
5. **T23 is not the limit.** A 2025 T23 vendor driver pushes 134 Mpix/s active at 743 Mbps × 2 through the T23 CSI and ISP at the default `isp_clk` 200 MHz. The SC2336 maxes out at about 81-84 Mpix/s, and a small ROI at high fps carries even fewer pixels. One remaining risk is per-frame overhead in the ISP/IMP pipeline at more than 100 fps (frame-done interrupts, AE/AWB per frame, encoder). No T23 driver above 60 fps exists except the old os02k10/os02d20 640x480@120 tables, whose T23 testing status is unknown.
6. **For a lightgun**, a centre crop narrows the FOV. A 640x480 crop of a 1920-wide sensor sees one third of the horizontal field. A **full-width strip** (for example 1920x240) keeps the horizontal FOV and still gains rate through VTS. Lens and geometry decide which is usable.

### Theoretical frame rates: fps = pclk / (HTS × VTS)

| window (all crops are centre crops unless noted) | type | HTS | VTS (active+30 / +45) | 81 MHz | 84 MHz | evidence |
|---|---|---|---|---|---|---|
| 1920x1080 | full FOV | 2250 | 1110 / 1125 | 32.4 / 32.0 | 33.6 / 33.2 | 30 fps published (VTS 1200) |
| 1920x1080 | full FOV | 2200 | 1110 / 1125 | 33.2 / 32.7 | 34.4 / 33.9 | HTS 2200 + VTS 1125 published at 74.25 MHz (SigmaStar) |
| 1280x720 | crop | 1800 | 750 / 765 | **60.0** / 58.8 | 62.2 / 61.0 | **60 fps published (ESP)** |
| 960x540 | crop | 1800 | 570 / 585 | 78.9 / 76.9 | 81.9 / 79.8 | inferred (HTS 1800 proven) |
| 960x540 | crop | 1600 / 1400 | 570 | 88.8 / 101.5 | 92.1 / 105.3 | speculative HTS |
| 800x600 | crop | 1800 | 630 / 645 | 71.4 / 69.8 | 74.1 / 72.4 | inferred |
| 800x600 | crop | 1600 / 1400 | 630 | 80.4 / 91.8 | 83.3 / 95.2 | speculative HTS |
| 640x480 | crop | 1800 | 510 / 525 | **88.2** / 85.7 | 91.5 / 88.9 | inferred (50 fps published at 42 MHz) |
| 640x480 | crop | 1600 / 1400 | 510 | 99.3 / 113.4 | 102.9 / 117.6 | speculative HTS |
| 640x360 | crop | 1800 / 1600 | 390 | 115.4 / 129.8 | 119.7 / 134.6 | inferred / speculative |
| 1920x240 strip | full width, V crop | 2250 | 270 / 285 | 133.3 / 126.3 | 138.3 / 131.0 | inferred |
| 320x240 ROI | crop | 1800 / 1600 | 270 | 166.7 / 187.5 | 172.8 / 194.4 | inferred / speculative |
| 640x120 ROI | crop | 1800 | 150 / 165 | 300 / 273 | 311 / 283 | inferred; blanking and AE may dominate |

At 84 MHz RAW10 the data rate is 420 Mbps/lane, within what the T23 CSI runs elsewhere (720-1080 Mbps). Short VTS also caps the maximum exposure. For example, VTS 510 at 22.2 µs per line gives at most about 11.2 ms of exposure, which suits IR-LED tracking.

---

## (c) Unknowns

* SC2336 minimum HTS for a given window width, minimum vertical blanking, and the maximum internal ADC/row rate: no datasheet available.
* The PLL formula behind 0x36e9-0x36ed and 0x37f9-0x37fd: SmartSens does not publish it. Observed sets: 81 MHz = 09 0c 1c 28 / 09 32 11 37 (53/53); 84 MHz RAW8 = 0e 0a 1a 18 / 15 32 11 17 (54/54); 72 MHz RAW8 = 08 0a 1a 18 / 08 32 11 37 (53/53); 67.2 MHz RAW10 = 0e 0c 1c 18 / 15 32 11 17 (54/30); 42 MHz = 07 1c 1c 28 / 07 32 11 37 (24/20); default 66 MHz = no writes, 20/27. Patterns: 0x36eb 0x0a↔RAW8 and 0x0c↔RAW10; 0x37f9 appears to carry a post-divider. Not enough to safely extrapolate above 84 MHz.
* Whether SC2336 (0xCB3A) supports the SmartSens skip/bin registers 0x3215/0x3220/0x5000/0x5900/0x5901.
* Whether the 0x3395 and 0x3641 changes in 720p60 are timing-critical (ADC/row timing) or cosmetic. Keep them.
* T23N-specific ISP limits: maximum fps and pixel rate, minimum frame size, and minimum horizontal/vertical blanking the CSI or ISP accepts. tx-isp-t23 is closed source. The empirical evidence is the vendor drivers in A4.
* Whether the SDK's 2019/2020 high-fps T23 drivers (os02k10 640x480@120 etc.) were ever validated on T23 silicon.
* No public report was found of anyone running SC2336 above 60 fps, or the T23 above 30 fps at 1080p or 60 fps at 720p with this sensor.

---

## Porting notes for the Ingenic driver (t23_sc2336.c)

* For a new mode, set `sensor_attr.mipi.image_twidth/theight` to the window. Set `total_width` = HTS and `total_height` = VTS, plus `max_integration_time*` = VTS-4 (or -6 to match ESP). Raise `SENSOR_OUTPUT_MAX_FPS` (`:29`). Add the mode to `sensor_win_sizes` with `.fps = N<<16|1`. `set_fps` already uses 81 MHz and reads HTS back, so it works unchanged at HTS 1800.
* MIPI `.clk = 405, .lans = 2` is unchanged for the 720p60 port and for derived 81 MHz crops.
* The ESP tables do not end with `{0x0100, 0x01}`; the ESP driver streams on separately. Ingenic tables include it. Append it if your stream-on path expects it.
* Use the ISP/IMP framesource at native size. Requesting ISP downscale does not raise sensor fps.

---

## Register tables

### T1. ESP `sc2336_mipi_2lane_24Minput_1280x720_raw10_60fps.h` (verbatim, converted to Ingenic syntax)
Source: https://raw.githubusercontent.com/espressif/esp-video-components/08bcabdf99c7a6f4e5b7e03119ea389a226fc7d2/esp_cam_sensor/sensors/sc2336/private_include/sc2336_mipi_2lane_24Minput_1280x720_raw10_60fps.h
24 MHz MCLK, 2-lane, 405 Mbps, RAW10, pclk 81 MHz, HTS 1800, VTS 750, centre crop. Same PLL as stock t23_sc2336.c.

```c
static struct regval_list sensor_init_regs_1280_720_60fps_mipi[] = {
	{0x0103, 0x01},
	{0x0100, 0x00},
	{0x36e9, 0x80},
	{0x37f9, 0x80},
	{0x301f, 0x30},
	{0x3106, 0x05},
	{0x3200, 0x01},
	{0x3201, 0x34},
	{0x3202, 0x00},
	{0x3203, 0xb4},
	{0x3204, 0x06},
	{0x3205, 0x53},
	{0x3206, 0x03},
	{0x3207, 0x8b},
	{0x3208, 0x05},
	{0x3209, 0x00},
	{0x320a, 0x02},
	{0x320b, 0xd0},
	{0x320c, 0x07},
	{0x320d, 0x08},
	{0x320e, 0x02},
	{0x320f, 0xee},
	{0x3210, 0x00},
	{0x3211, 0x10},
	{0x3212, 0x00},
	{0x3213, 0x04},
	{0x3248, 0x04},
	{0x3249, 0x0b},
	{0x3253, 0x08},
	{0x3301, 0x09},
	{0x3302, 0xff},
	{0x3303, 0x10},
	{0x3306, 0x60},
	{0x3307, 0x02},
	{0x330a, 0x01},
	{0x330b, 0x10},
	{0x330c, 0x16},
	{0x330d, 0xff},
	{0x3318, 0x02},
	{0x3321, 0x0a},
	{0x3327, 0x0e},
	{0x332b, 0x12},
	{0x3333, 0x10},
	{0x3334, 0x40},
	{0x335e, 0x06},
	{0x335f, 0x0a},
	{0x3364, 0x1f},
	{0x337c, 0x02},
	{0x337d, 0x0e},
	{0x3390, 0x09},
	{0x3391, 0x0f},
	{0x3392, 0x1f},
	{0x3393, 0x20},
	{0x3394, 0x20},
	{0x3395, 0x30},
	{0x33a2, 0x04},
	{0x33b1, 0x80},
	{0x33b2, 0x68},
	{0x33b3, 0x42},
	{0x33f9, 0x70},
	{0x33fb, 0xd0},
	{0x33fc, 0x0f},
	{0x33fd, 0x1f},
	{0x349f, 0x03},
	{0x34a6, 0x0f},
	{0x34a7, 0x1f},
	{0x34a8, 0x42},
	{0x34a9, 0x06},
	{0x34aa, 0x01},
	{0x34ab, 0x23},
	{0x34ac, 0x01},
	{0x34ad, 0x84},
	{0x3630, 0xf4},
	{0x3633, 0x22},
	{0x3639, 0xf4},
	{0x363c, 0x47},
	{0x3641, 0x03}, // PAD driving
	{0x3670, 0x09},
	{0x3674, 0xf4},
	{0x3675, 0xfb},
	{0x3676, 0xed},
	{0x367c, 0x09},
	{0x367d, 0x0f},
	{0x3690, 0x33},
	{0x3691, 0x33},
	{0x3692, 0x43},
	{0x3698, 0x89},
	{0x3699, 0x96},
	{0x369a, 0xd0},
	{0x369b, 0xd0},
	{0x369c, 0x09},
	{0x369d, 0x0f},
	{0x36a2, 0x09},
	{0x36a3, 0x0f},
	{0x36a4, 0x1f},
	{0x36d0, 0x01},
	{0x36ea, 0x09},
	{0x36eb, 0x0c},
	{0x36ec, 0x1c},
	{0x36ed, 0x28},
	{0x3722, 0xe1},
	{0x3724, 0x41},
	{0x3725, 0xc1},
	{0x3728, 0x20},
	{0x37fa, 0x09},
	{0x37fb, 0x32},
	{0x37fc, 0x11},
	{0x37fd, 0x37},
	{0x3900, 0x0d},
	{0x3905, 0x98},
	{0x391b, 0x81},
	{0x391c, 0x10},
	{0x3933, 0x81},
	{0x3934, 0xc5},
	{0x3940, 0x68},
	{0x3941, 0x00},
	{0x3942, 0x01},
	{0x3943, 0xc6},
	{0x3952, 0x02},
	{0x3953, 0x0f},
	{0x3e01, 0x37},
	{0x3e02, 0xe0},
	{0x3e08, 0x1f},
	{0x3e1b, 0x14},
	{0x440e, 0x02},
	{0x4509, 0x38},
	{0x4819, 0x06},
	{0x481b, 0x03},
	{0x481d, 0x0b},
	{0x481f, 0x03},
	{0x4821, 0x08},
	{0x4823, 0x03},
	{0x4825, 0x03},
	{0x4827, 0x03},
	{0x4829, 0x05},
	{0x5799, 0x06},
	{0x5ae0, 0xfe},
	{0x5ae1, 0x40},
	{0x5ae2, 0x30},
	{0x5ae3, 0x28},
	{0x5ae4, 0x20},
	{0x5ae5, 0x30},
	{0x5ae6, 0x28},
	{0x5ae7, 0x20},
	{0x5ae8, 0x3c},
	{0x5ae9, 0x30},
	{0x5aea, 0x28},
	{0x5aeb, 0x3c},
	{0x5aec, 0x30},
	{0x5aed, 0x28},
	{0x5aee, 0xfe},
	{0x5aef, 0x40},
	{0x5af4, 0x30},
	{0x5af5, 0x28},
	{0x5af6, 0x20},
	{0x5af7, 0x30},
	{0x5af8, 0x28},
	{0x5af9, 0x20},
	{0x5afa, 0x3c},
	{0x5afb, 0x30},
	{0x5afc, 0x28},
	{0x5afd, 0x3c},
	{0x5afe, 0x30},
	{0x5aff, 0x28},
	{0x36e9, 0x53},
	{0x37f9, 0x53},
	{SENSOR_REG_END, 0x00},
};
```

### T2. ESP `sc2336_mipi_2lane_24Minput_640x480_raw10_50fps.h` (verbatim, converted)
Source: same directory, `sc2336_mipi_2lane_24Minput_640x480_raw10_50fps.h`.
2-lane, **210 Mbps** (different PLL), pclk 42 MHz, HTS 1600, VTS 525, centre crop. On T23 set `mipi.clk = 210`. This table is mainly useful as the source of the 640x480 window registers and as a working small-window reference.

```c
static struct regval_list sensor_init_regs_640_480_50fps_mipi[] = {
	{0x0103, 0x01},
	{0x0100, 0x00},
	{0x36e9, 0x80},
	{0x37f9, 0x80},
	{0x301f, 0xa8},
	{0x3106, 0x05},
	{0x3200, 0x02},
	{0x3201, 0x74},
	{0x3202, 0x01},
	{0x3203, 0x2c},
	{0x3204, 0x05},
	{0x3205, 0x13},
	{0x3206, 0x03},
	{0x3207, 0x13},
	{0x3208, 0x02},
	{0x3209, 0x80},
	{0x320a, 0x01},
	{0x320b, 0xe0},
	{0x320c, 0x06},
	{0x320d, 0x40},
	{0x320e, 0x02},
	{0x320f, 0x0d},
	{0x3210, 0x00},
	{0x3211, 0x10},
	{0x3212, 0x00},
	{0x3213, 0x04},
	{0x3248, 0x04},
	{0x3249, 0x0b},
	{0x3250, 0x40},
	{0x3253, 0x08},
	{0x3301, 0x09},
	{0x3302, 0xff},
	{0x3303, 0x10},
	{0x3306, 0x60},
	{0x3307, 0x02},
	{0x330a, 0x01},
	{0x330b, 0x10},
	{0x330c, 0x16},
	{0x330d, 0xff},
	{0x3318, 0x02},
	{0x3321, 0x0a},
	{0x3327, 0x0e},
	{0x332b, 0x12},
	{0x3333, 0x10},
	{0x3334, 0x40},
	{0x335e, 0x06},
	{0x335f, 0x0a},
	{0x3364, 0x1f},
	{0x337c, 0x02},
	{0x337d, 0x0e},
	{0x3390, 0x09},
	{0x3391, 0x0f},
	{0x3392, 0x1f},
	{0x3393, 0x20},
	{0x3394, 0x20},
	{0x3395, 0x20},
	{0x33a2, 0x04},
	{0x33b1, 0x80},
	{0x33b2, 0x68},
	{0x33b3, 0x42},
	{0x33f9, 0x78},
	{0x33fb, 0xd0},
	{0x33fc, 0x0f},
	{0x33fd, 0x1f},
	{0x349f, 0x03},
	{0x34a6, 0x0f},
	{0x34a7, 0x1f},
	{0x34a8, 0x42},
	{0x34a9, 0x06},
	{0x34aa, 0x01},
	{0x34ab, 0x28},
	{0x34ac, 0x01},
	{0x34ad, 0x84},
	{0x3630, 0xf4},
	{0x3633, 0x22},
	{0x3639, 0xf4},
	{0x363c, 0x47},
	{0x3670, 0x09},
	{0x3674, 0xf4},
	{0x3675, 0xfb},
	{0x3676, 0xed},
	{0x367c, 0x09},
	{0x367d, 0x0f},
	{0x3690, 0x22},
	{0x3691, 0x22},
	{0x3692, 0x22},
	{0x3698, 0x89},
	{0x3699, 0x96},
	{0x369a, 0xd0},
	{0x369b, 0xd0},
	{0x369c, 0x09},
	{0x369d, 0x0f},
	{0x36a2, 0x09},
	{0x36a3, 0x0f},
	{0x36a4, 0x1f},
	{0x36d0, 0x01},
	{0x36ea, 0x07},
	{0x36eb, 0x1c},
	{0x36ec, 0x1c},
	{0x36ed, 0x28},
	{0x3722, 0xe1},
	{0x3724, 0x41},
	{0x3725, 0xc1},
	{0x3728, 0x20},
	{0x37fa, 0x07},
	{0x37fb, 0x32},
	{0x37fc, 0x11},
	{0x37fd, 0x37},
	{0x3900, 0x0d},
	{0x3905, 0x98},
	{0x391b, 0x81},
	{0x391c, 0x10},
	{0x3933, 0x81},
	{0x3934, 0xc5},
	{0x3940, 0x68},
	{0x3941, 0x00},
	{0x3942, 0x01},
	{0x3943, 0xc6},
	{0x3952, 0x02},
	{0x3953, 0x0f},
	{0x3e01, 0x20},
	{0x3e02, 0x70},
	{0x3e08, 0x1f},
	{0x3e1b, 0x14},
	{0x440e, 0x02},
	{0x4509, 0x38},
	{0x4800, 0x44},
	{0x4819, 0x03},
	{0x481b, 0x02},
	{0x481d, 0x06},
	{0x481f, 0x02},
	{0x4821, 0x07},
	{0x4823, 0x02},
	{0x4825, 0x02},
	{0x4827, 0x02},
	{0x4829, 0x03},
	{0x5799, 0x06},
	{0x5ae0, 0xfe},
	{0x5ae1, 0x40},
	{0x5ae2, 0x30},
	{0x5ae3, 0x28},
	{0x5ae4, 0x20},
	{0x5ae5, 0x30},
	{0x5ae6, 0x28},
	{0x5ae7, 0x20},
	{0x5ae8, 0x3c},
	{0x5ae9, 0x30},
	{0x5aea, 0x28},
	{0x5aeb, 0x3c},
	{0x5aec, 0x30},
	{0x5aed, 0x28},
	{0x5aee, 0xfe},
	{0x5aef, 0x40},
	{0x5af4, 0x30},
	{0x5af5, 0x28},
	{0x5af6, 0x20},
	{0x5af7, 0x30},
	{0x5af8, 0x28},
	{0x5af9, 0x20},
	{0x5afa, 0x3c},
	{0x5afb, 0x30},
	{0x5afc, 0x28},
	{0x5afd, 0x3c},
	{0x5afe, 0x30},
	{0x5aff, 0x28},
	{0x36e9, 0x24},
	{0x37f9, 0x20},
	{SENSOR_REG_END, 0x00},
};
```

### T3. DERIVED, UNTESTED: 640x480 crop at 81 MHz, HTS 1800, VTS 510 → 88.2 fps
This is my construction, published nowhere. It is T1 (the 405 Mbps PLL is unchanged, so MIPI `.clk = 405`) with the T2 window and VTS 510. If it fails, try VTS 525 first (85.7 fps) and then HTS 1800 → 2250. Exposure max is about 504 lines (11.2 ms).

```c
static struct regval_list sensor_init_regs_640_480_88fps_mipi_UNTESTED[] = {
	{0x0103, 0x01},
	{0x0100, 0x00},
	{0x36e9, 0x80},
	{0x37f9, 0x80},
	{0x301f, 0x30},
	{0x3106, 0x05},
	{0x3200, 0x02},
	{0x3201, 0x74}, // x_start 0x274 = 628 (from esp 640x480 table)
	{0x3202, 0x01},
	{0x3203, 0x2c}, // y_start 0x12c = 300
	{0x3204, 0x05},
	{0x3205, 0x13}, // x_end 0x513 = 1299
	{0x3206, 0x03},
	{0x3207, 0x13}, // y_end 0x313 = 787 (0x3206 stays 0x03)
	{0x3208, 0x02},
	{0x3209, 0x80}, // out width 640
	{0x320a, 0x01},
	{0x320b, 0xe0}, // out height 480
	{0x320c, 0x07},
	{0x320d, 0x08},
	{0x320e, 0x01},
	{0x320f, 0xfe}, // VTS 0x1fe = 510 -> 81e6/(1800*510) = 88.2 fps
	{0x3210, 0x00},
	{0x3211, 0x10},
	{0x3212, 0x00},
	{0x3213, 0x04},
	{0x3248, 0x04},
	{0x3249, 0x0b},
	{0x3253, 0x08},
	{0x3301, 0x09},
	{0x3302, 0xff},
	{0x3303, 0x10},
	{0x3306, 0x60},
	{0x3307, 0x02},
	{0x330a, 0x01},
	{0x330b, 0x10},
	{0x330c, 0x16},
	{0x330d, 0xff},
	{0x3318, 0x02},
	{0x3321, 0x0a},
	{0x3327, 0x0e},
	{0x332b, 0x12},
	{0x3333, 0x10},
	{0x3334, 0x40},
	{0x335e, 0x06},
	{0x335f, 0x0a},
	{0x3364, 0x1f},
	{0x337c, 0x02},
	{0x337d, 0x0e},
	{0x3390, 0x09},
	{0x3391, 0x0f},
	{0x3392, 0x1f},
	{0x3393, 0x20},
	{0x3394, 0x20},
	{0x3395, 0x30},
	{0x33a2, 0x04},
	{0x33b1, 0x80},
	{0x33b2, 0x68},
	{0x33b3, 0x42},
	{0x33f9, 0x70},
	{0x33fb, 0xd0},
	{0x33fc, 0x0f},
	{0x33fd, 0x1f},
	{0x349f, 0x03},
	{0x34a6, 0x0f},
	{0x34a7, 0x1f},
	{0x34a8, 0x42},
	{0x34a9, 0x06},
	{0x34aa, 0x01},
	{0x34ab, 0x23},
	{0x34ac, 0x01},
	{0x34ad, 0x84},
	{0x3630, 0xf4},
	{0x3633, 0x22},
	{0x3639, 0xf4},
	{0x363c, 0x47},
	{0x3641, 0x03}, // PAD driving
	{0x3670, 0x09},
	{0x3674, 0xf4},
	{0x3675, 0xfb},
	{0x3676, 0xed},
	{0x367c, 0x09},
	{0x367d, 0x0f},
	{0x3690, 0x33},
	{0x3691, 0x33},
	{0x3692, 0x43},
	{0x3698, 0x89},
	{0x3699, 0x96},
	{0x369a, 0xd0},
	{0x369b, 0xd0},
	{0x369c, 0x09},
	{0x369d, 0x0f},
	{0x36a2, 0x09},
	{0x36a3, 0x0f},
	{0x36a4, 0x1f},
	{0x36d0, 0x01},
	{0x36ea, 0x09},
	{0x36eb, 0x0c},
	{0x36ec, 0x1c},
	{0x36ed, 0x28},
	{0x3722, 0xe1},
	{0x3724, 0x41},
	{0x3725, 0xc1},
	{0x3728, 0x20},
	{0x37fa, 0x09},
	{0x37fb, 0x32},
	{0x37fc, 0x11},
	{0x37fd, 0x37},
	{0x3900, 0x0d},
	{0x3905, 0x98},
	{0x391b, 0x81},
	{0x391c, 0x10},
	{0x3933, 0x81},
	{0x3934, 0xc5},
	{0x3940, 0x68},
	{0x3941, 0x00},
	{0x3942, 0x01},
	{0x3943, 0xc6},
	{0x3952, 0x02},
	{0x3953, 0x0f},
	{0x3e01, 0x37},
	{0x3e02, 0xe0},
	{0x3e08, 0x1f},
	{0x3e1b, 0x14},
	{0x440e, 0x02},
	{0x4509, 0x38},
	{0x4819, 0x06},
	{0x481b, 0x03},
	{0x481d, 0x0b},
	{0x481f, 0x03},
	{0x4821, 0x08},
	{0x4823, 0x03},
	{0x4825, 0x03},
	{0x4827, 0x03},
	{0x4829, 0x05},
	{0x5799, 0x06},
	{0x5ae0, 0xfe},
	{0x5ae1, 0x40},
	{0x5ae2, 0x30},
	{0x5ae3, 0x28},
	{0x5ae4, 0x20},
	{0x5ae5, 0x30},
	{0x5ae6, 0x28},
	{0x5ae7, 0x20},
	{0x5ae8, 0x3c},
	{0x5ae9, 0x30},
	{0x5aea, 0x28},
	{0x5aeb, 0x3c},
	{0x5aec, 0x30},
	{0x5aed, 0x28},
	{0x5aee, 0xfe},
	{0x5aef, 0x40},
	{0x5af4, 0x30},
	{0x5af5, 0x28},
	{0x5af6, 0x20},
	{0x5af7, 0x30},
	{0x5af8, 0x28},
	{0x5af9, 0x20},
	{0x5afa, 0x3c},
	{0x5afb, 0x30},
	{0x5afc, 0x28},
	{0x5afd, 0x3c},
	{0x5afe, 0x30},
	{0x5aff, 0x28},
	{0x36e9, 0x53},
	{0x37f9, 0x53},
	{SENSOR_REG_END, 0x00},
};
```

For other crops with T3 as the base, the ESP window registers follow an exact pattern. It reproduces the 1280x720, 640x480 and 800x800 values exactly. Every window is centred at x = 963.5, y = 543.5, with 32 extra columns and 8 extra rows, and offsets 0x3210=0x10 and 0x3212=0x04:
`x_start = 948 - W/2; x_end = x_start + W + 31; y_start = 540 - H/2; y_end = y_start + H + 7`. Write 0x3208/0x320a = W/H, then set VTS >= H + 30.

## Sources
* espressif/esp-video-components `esp_cam_sensor/sensors/sc2336/` @08bcabdf: https://github.com/espressif/esp-video-components/tree/master/esp_cam_sensor/sensors/sc2336
* Ingenic SDK SC2336 drivers: local `scratchpad/sdk-sc2336/*.c`
* themactep/ingenic-sdk `common/sensor/t23/` @9bb8ad51: https://github.com/themactep/ingenic-sdk/tree/master/common/sensor/t23
* Rockchip sc2336.c: https://raw.githubusercontent.com/unifreq/linux-6.1.y-rockchip/HEAD/drivers/media/i2c/sc2336.c
* Rockchip sc200ai.c / sc450ai.c (binning reference): https://raw.githubusercontent.com/rockchip-linux/kernel/develop-5.10/drivers/media/i2c/sc200ai.c and the matching sc450ai.c
* SigmaStar: https://raw.githubusercontent.com/OpenIPC/sensors/HEAD/sigmastar/infinity6b0/sensor_sc2336_mipi.c
* Sophgo: https://github.com/sophgo/cvi_alios_open/tree/HEAD/components/cvi_sensor/sms_sc2336
* Sipeed BL808 (empty): https://raw.githubusercontent.com/sipeed/M1s_BL808_SDK/HEAD/components/stage/sensor/include/sc2336.h
* Adafruit: https://github.com/adafruit/Adafruit_CircuitPython_SC2336
* GoPhotonics spec listing: https://www.gophotonics.com/products/cmos-image-sensors/smartsens-technology/21-1025-sc2336
* OpenIPC/firmware#1324 (SC223A, not relevant): https://github.com/OpenIPC/firmware/issues/1324
* T23ZN CSDN post (unverified): https://damodev.csdn.net/6aa3cf35caa31c02abbefeba.html
* thingino: `Config.soc.in:188-191`, `thingino.mk:322-327`, `configs/cameras/sonoff_s2_t23n_sc2336_atbm6012bx/`

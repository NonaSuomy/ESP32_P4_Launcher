# ESP32-P4 chip v1.x vs v3.2: performance

Two Guition JC1060P470C 7" boards, the same apart from the chip: `guition_p4_7` (chip v1.x, CPU at 360 MHz)
and `guition_p4_7_v3` (chip v3.2, CPU at 400 MHz, the 2026 panel). Both: 32 MB PSRAM at 200 MHz, launcher
0.32.0 + this branch, the same benchmark binary (except the 3D engine's SIMD build, see below).
Run on 22 Nov 2026 with `tools/perf_bench.py`.

## Summary

- **CPU: +13% on one core, +16% on two.** Mostly the clock (400 vs 360 MHz = +11%); per MHz the v3 core
  does 2% more CoreMark (3.57 vs 3.51 iterations/s per MHz).
- **Internal SRAM: +13%**, the clock again.
- **PSRAM: +15 to +23% bandwidth, load latency 634 → 523 ns.** More than the clock: the v3's memory
  path is a little better. Writing from both cores at once is only +8%.
- **The LCD refresh costs about a sixth of the PSRAM bandwidth on both chips.** With the scan-out
  stopped, PSRAM read goes up 18–19% and latency down 17%. CPU-only work (CoreMark, SRAM) doesn't change.
- **3D engine (e3d, 5,700 triangles a frame with shadows, 400x240 on 2 cores): +16 to +19%.** With the
  screen off the v1.x board gains 8% and the v3 board about 1%, so the refresh slows the v3 board's 3D less.
- **The engine's SIMD code adds about 8% on both chips**, but it has to be built per chip generation
  (finding 1).

## Results

Screen off = the panel's scan-out stopped (`papp_dev.py display off`): the frame DMA no longer reads
1024x600x2 bytes from PSRAM 60 times a second, about 74 MB/s.

| Test | v1.x screen on | v1.x screen off | v3.2 screen on | v3.2 screen off | v3 vs v1, screen on | v3 vs v1, screen off |
|---|---|---|---|---|---|---|
| CPU clock (MHz) | 360.0 |  | 400.0 |  | +11% |  |
| CoreMark, 1 core | 1,263.3 | 1,263.2 | 1,426.5 | 1,426.9 | +13% | +13% |
| CoreMark, 2 cores | 2,436.3 | 2,437.4 | 2,830.1 | 2,831.9 | +16% | +16% |
| CoreMark/MHz, 1 core | 3.51 | 3.51 | 3.57 | 3.57 | +2% | +2% |
| PSRAM read, 1 core (MB/s) | 88.7 | 104.3 | 106.8 | 127.0 | +20% | +22% |
| PSRAM write, 1 core (MB/s) | 64.0 | 78.8 | 73.5 | 91.4 | +15% | +16% |
| PSRAM memcpy, read+write (MB/s) | 83.5 | 100.2 | 100.8 | 121.8 | +21% | +22% |
| PSRAM read, 2 cores (MB/s) | 107.6 | 128.2 | 132.4 | 160.2 | +23% | +25% |
| PSRAM write, 2 cores (MB/s) | 57.4 | 68.5 | 62.1 | 74.8 | +8% | +9% |
| PSRAM load latency (ns) | 633.9 | 529.6 | 522.6 | 433.1 | +21% | +22% |
| SRAM read (MB/s) | 620.7 | 622.0 | 702.8 | 703.2 | +13% | +13% |
| SRAM write (MB/s) | 1,121.7 | 1,120.6 | 1,265.2 | 1,266.2 | +13% | +13% |
| SRAM memcpy (MB/s) | 743.7 | 744.0 | 840.0 | 840.9 | +13% | +13% |
| SRAM load latency (ns) | 14.3 | 14.3 | 12.6 | 12.6 | +13% | +13% |
| PPA fill 1024x600 (Mpix/s) | 176.3 |  | 191.3 |  | +9% |  |
| PPA scale 2x to 1024x600 (Mpix/s) | 14.4 |  | 16.6 |  | +15% |  |
| 3D (engine in C), unseen buffer (fps) | 42.3 | 45.6 | 50.3 | 50.6 | +19% | +11% |
| 3D render time, unseen (ms/frame) | 23.6 | 21.9 | 19.8 | 19.7 | +19% | +11% |
| 3D (engine in C) on screen (fps) | 37.2 |  | 43.3 |  | +16% |  |
| 3D with the chip's SIMD, unseen buffer (fps) | 45.8 | 49.5 | 54.2 | 54.3 | +18% | +10% |
| 3D with SIMD on screen (fps) | 39.4 |  | 45.9 |  | +16% |  |

Percentages: how much better the v3 board does (for latency and times too, positive = faster).
On screen = through a 2-buffer surface that the PPA scales to 1024x600, as the 3D games do.

## Findings beyond the numbers

### 1. Code built with the v1.x PIE (SIMD) extension crashes on a v3 chip

ESP-IDF builds v1.x chips with `-march=..._xesppie` and v3.x chips with `..._xespv` (+ `_xesploop`):
the same instruction names with **different encodings** (`esp.movx.w.sar a1` is 0x90b1805f for v1.x
and 0x90c1a01b for v3; `components/soc/project_include.cmake` in IDF picks one by
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`). A v1.x PIE instruction on a v3.2 chip is decoded as something else:
here a store through `a1`, "store access fault, mtval=0xe", and the board restarts.

- The e3d engine builds `e3d_render.c` with `_xesppie` (`ESP32_P4_3D_Engine/examples/papp/papp_common.py`),
  so **every 3D game built with it (Shardwall, Voidmarch, Driftrock, Dustline, Gorgewing, Tristrata) and
  the e3d examples crash on a v3 board** as soon as the engine starts (in its PIE self-check).
- The same source builds and runs correctly with `-march=..._xespv -D__riscv_xesppie=1` (the engine's
  self-check agrees with its C code), so the engine needs no rewrite, only a build per generation and a
  way to pick one.
- It goes the other way too: the `_xespv` build crashes on the v1.x board (in the lighting kernel).
- **Fixed (launcher 0.33 + engine):** SYS v4 `papp_chip_rev()` gives apps the chip revision. The engine
  builds its SIMD code (lighting, fill, copy: `e3d/src/e3d_simd_kernels.inc`) twice, `e3d_simd_pie.c`
  and `e3d_simd_espv.c`, and the first `e3d_create` picks the set for the chip (log line
  `e3d: SIMD: PIE, chip v1.x` / `xespv, chip v3.x`). The rest is plain RV32, so **one .papp runs on
  both**: 3D 45.95 fps on v1.x (42.5 in C), 53.9 on v3.2 (50.0 in C), the same as the per-chip builds.
  An older launcher (no SYS v4) gets the C paths on every chip. The games still need rebuilding with
  the new engine and publishing.
- The SDK builds apps without SIMD (`-march=rv32imafc_zicsr_zifencei`), so ordinary apps run on both.
  GBA's `rv32_affine_pie.S` is written for `xespv` and isn't in its build.

### 2. On the v3 board, SD writes fail and the card stops writing until a reboot

Uploading an app right after the previous one ended (the launcher resumes the Wi-Fi then) or in the
first ~30 s after boot (Store catalogue, NTP, update check) fails on the v3 board, at the first try:
`sdmmc_req: handle_idle_state_events unhandled: 00000100`, then every SD write fails with 0x107
(timeout) until a reboot. 12 back-to-back runs on the v1.x board: no failure. It happens with the C6
link at 40 MHz as well as 10 MHz, so it isn't the new panel's clock setting. The SD card and the C6
share the SDMMC host (slots 0 and 1). The other difference: this board's C6 still has Guition's
firmware 2.3.2 (the boot log says "Version mismatch: Host [2.12.0] > Co-proc [2.3.0] ==> Upgrade
co-proc to avoid RPC timeouts"), the v1.x board has 2.12.13.

Trying to update the C6 (copying its 1.2 MB image to `/sd/update/c6.bin`) showed it is **not the C6
link**: the copy fails the same way with the Wi-Fi paused (an app running), 90 s after boot, and with
the SD card at 20 MHz instead of 40. After a fresh boot two 64 KB files write, the third times out;
at 20 MHz even the first. Reads, deletes and small writes keep working.

Narrowed down further:

- **Not the card.** With the two boards' cards swapped, the v3 board's card ("SDABC", 29.8 GB) writes
  fine in the v1.x board (8 × 64 KB + 1.2 MB), and the v1.x board's card fails in the v3 board on the
  2nd file. The failure stays with the board.
- **Not the C6 firmware.** The C6 is now on 2.12.13 (Settings → Update, from `/sd/update/c6.bin`).
  The first 64 KB write 54 s after boot still fails. The card is then stuck in the receive state
  (status 0x400d00: "rcv", ready for data, illegal command), so the host never finishes the data phase.
- **The 4-bit data lines.** With the slot mounted **1-bit** (a temporary build) everything writes:
  8 × 64 KB + 1.2 MB, read back identical. It is slow, 205–245 KB/s. Only D0 is used then, so the fault
  is most likely on D1–D3 of this board: a solder joint or the slot's contacts. That fits writes having
  worked at the start of the evening. Reads seem to tolerate it better than writes.

**Fallback, now in the launcher** (`components/fatfs/README_LAUNCHER.md`). A 4-bit transfer that times
out moves the card to 1-bit (STOP by hand, then ACMD6) and is repeated. On this board, the first
64 KB file takes 5.4 s (one timeout), then everything writes and reads back identical, the C6 keeps
running, and Settings shows "1-bit (4-bit failed)" (DevLink `info`: `sd_1bit`). The v1.x board stays
4-bit. Still worth doing: look at the slot and D1–D3 under a magnifier, or try a second v3.2 7" board.

### 3. Smaller notes

- Writing PSRAM from both cores at once is slower than from one on both chips (v1.x 57 vs 64 MB/s).
- The thread priority trap from mandel_race applies to benchmarks too: a HIGH-priority worker pinned to
  the creating thread's core runs to its end before the creator starts the next worker, so a "2-core"
  test silently runs one core at a time.

## How it works / running it again

```powershell
python tools\perf_bench.py run COM10 COM6 --settle 25     # builds, runs both boards, writes the report
python tools\perf_bench.py report                         # the table again from build\perf_bench\*.json
```

- `apps/perf_bench/main.c`: the tests, one per run (`clock`, `cpu`, `mem`, `ppa`, `3d off|show [frames]`),
  results as `RESULT <name> <value> <unit>` log lines.
  - CoreMark (EEMBC, `apps/perf_bench/coremark/`, Apache 2.0, commit in `VERSION.txt`): the
    performance run (`-O3 -funroll-all-loops`), at least 10 s; 2 cores = 2 contexts in PAPP threads,
    one per core.
  - Memory: 8 MB PSRAM / 64 KB SRAM buffers, kernels in FAST memory, best of 3; latency = random
    pointer chase, one hop per cache line.
  - 3D: the e3d engine (from `ESP32_P4_3D_Engine` beside the checkout, or `E3D_DIR`), a fixed scene
    (floor, 9 spheres, a gem, a moving light, shadows), frame N at t = N/30 s on every board.
  - Built three times: `perf_bench` (engine in C), `perf_bench_pie` (v1.x SIMD), `perf_bench_espv`
    (v3 SIMD). `tools/perf_bench.py` runs the one that fits the board.
- Screen off: DevLink `display off|on` (`papp_dev.py display off`) → `bsp_display_scanout()`: the DSI
  host's pattern generator takes over, so the frame DMA stops reading PSRAM, and the backlight goes off.
  Only the Guition 7" boards have it so far (others answer "not supported").
- `--settle 25` waits after each test while the launcher resumes the Wi-Fi; if the SD card fails anyway
  (finding 2) the script reboots the board and repeats that test once.

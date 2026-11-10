# Education apps — plan

More apps for the **Education** folder (`apps/education/<app>/`, ids `edu.*`, never in the Store).
Each one teaches one idea, shows it on screen and lets the student change it live.

Status (2026-11-10): all six apps below are built, 0.1.0, on the starter card. Before them: BLE Remote,
Wi-Fi Remote, Sprite Demo, LVGL Demo.

| App | Measured on the 7" | Notes |
|---|---|---|
| Game of Life | 18 fps full redraw | Grid 64x44 (not 100x60: room for the side panel) |
| Mandelbrot Race | 1 core 595 ms, 2 cores 299 ms (2.0x), FAST 298 ms, fixed point 345 ms | Workers must run at NORMAL priority (HIGH starves the main thread on its core: no speed-up) |
| Synth | 32 kHz mono, 0 underruns | Keyboard instead of a frequency slider; echo; FFT 512 |
| Touch Paint | flood fill of the canvas 26 ms | `selftest` argument checks fill + save |
| Physics Playground | 28.6 fps with 12 balls (the 7"'s cap for an 800x480 2-buffer surface) | Speed arrows not done; energy bar instead |
| Sensor Dashboard | a sample every ~7 s (Wi-Fi scan + 3 s BLE scan) | No "CPU time per frame" tile; one chart, tiles pick it |

4.3" and Tab5: all six run and exit with 0; Mandelbrot 662 / 586 ms on 1 core (x2.0 on both), Physics 27 / 24 fps.

Leaks: none, except 1 allocation after Paint saves or Dashboard records (newlib's stdio table, made by the first
`fopen`; the launcher frees it). Don't print floats with `%f` in apps: newlib's dtoa keeps its buffers
(4 allocations "leaked" per app); format tenths as integers instead.

## Common rules

- 800x480 surface, letterboxed by the compositor on every board (4.3", 7", Tab5).
- Touch and pad both work; **X exits** (also a close button). Pad on the 7" (DS4 mapping): injected B = A, Y = X.
- Use `edu_ui` for buttons, radios and the log panel, or LVGL (`edubuild.lvgl()`) when the app is mostly controls.
- Build with `apps/education/<app>/build.py`; `version.txt` 0.1.0; 88x88 `icon.png`; copied to `SDcard/apps/Education/<app>/`.
- Each app: an entry in `SDcard/README.md` (Education apps) and in `CLAUDE.md` (apps/education).
- Test on the 7" (COM10) first, then a look on the 4.3" (COM9) and the Tab5 (COM15). Exit code 0, no leaks.
- Apps on a launcher without a needed interface say so on screen instead of failing (as Sprite Demo's CPU fallback does).

## Apps

### 1. Mandelbrot race (`mandel_race`) — parallel work and memory speed
- Draws the same fractal three ways and times each: **1 core**, **2 cores** (`papp_thread_create`, rows split
  between threads), **2 cores + FAST** (the inner loop placed in internal SRAM, as `fast_bench` does).
- Bars with the time of each run and the speed-up (expect about 2x for threads).
- Touch to zoom in at a point, B to zoom out; float vs fixed-point switch as a 4th lesson.
- Interfaces: DISP, THRD, FAST placement. Risk: low.

### 2. Game of Life (`life`) — rules make patterns
- Grid (e.g. 100x60 cells of 8 px); touch draws or erases cells.
- Buttons: Play / Pause, Step, Clear, Random, speed slider; ready-made patterns (glider, blinker, glider gun).
- Generation counter and live-cell count.
- Interfaces: DISP only (fill or CPU). Risk: low. Good first app for students to read.

### 3. Synth and oscilloscope (`synth`) — what sound is
- Waveform radio: sine / square / triangle / saw / noise; sliders for frequency (50–2000 Hz) and volume.
- The wave being played is drawn live as an oscilloscope trace; a second view shows the spectrum (simple FFT).
- Pad buttons as piano notes (one octave) as a bonus.
- Interfaces: AUDI (`papp_audio_open` / `write`), DISP. Risk: low; check the 4.3" and Tab5 speakers.

### 4. Touch paint (`paint`) — files and multi-touch
- Brush sizes, colour palette, eraser, undo (a few steps), clear.
- Two fingers draw two lines at once (`papp_disp_touch` with several points).
- Save / Open as BMP on the card (`/sd/pictures/`), using the launcher's file browser (`papp_ui_pick_file`).
- Interfaces: DISP, FS, UI. Risk: low.

### 5. Physics playground (`physics`) — motion and collisions
- Balls with gravity, bounce and friction; flick one with touch to throw it.
- Sliders for gravity and bounciness; button to add balls; a "pause and show the speed arrows" switch.
- Balls drawn as ARGB8888 sprites by the PPA (`papp_disp_blit`), so many balls stay smooth.
- Interfaces: DISP v4. Risk: medium (performance with many balls; CPU fallback for older launchers).

### 6. Sensor dashboard (`dashboard`) — live data
- LVGL charts fed by real data: Wi-Fi signal strength over time, number of networks nearby,
  BLE devices nearby (scan), free memory, CPU time per frame.
- Tabs per source; a "log to the card" switch writing CSV for a spreadsheet lesson.
- Interfaces: NET, BLE, LVGL, FS. Risk: medium (radio use; NET and BLE together on one C6).

## Later ideas
- **Snake / Pong for two players** — game loop, input, score (two pads).
- **Morse code** — key with A, hear it (AUDI), decode on screen; or send it between two boards over BLE.
- **Binary / hex trainer** — flip bits with touch, see the number in decimal, hex and as a colour (RGB565).
- **Sorting visualiser** — bars sorted by bubble / insertion / quick sort, step by step, with counters.
- **PPA text** — A8 font glyphs drawn by the PPA in colour (DISP v4 A8 format).

## Launcher-side follow-ups these may need
- PPA batching (several blits in one call) if Physics playground is limited by call overhead.
- An FFT helper is app-side (no launcher change).

## Suggested order
1. Game of Life and Mandelbrot race (DISP and THRD only, quick wins).
2. Synth and Touch paint.
3. Physics playground, then Sensor dashboard.

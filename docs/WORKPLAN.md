# ESP32-P4 Launcher — Work Plan

> Companion to [`ARCHITECTURE.md`](ARCHITECTURE.md). Draft v0.1, 2026-10-29.
> Estimates are in **person-weeks (pw)** for one developer familiar with the reference project.
> Target board for Phases 0–7: the reference Guition ESP32-P4 4.3″ (LCD), with the HDMI variant as a
> second board config. Toolchain: ESP-IDF v5.5.2, `riscv32-esp-elf-gcc`, Windows/PowerShell + Python.

---

## Status (2026-11-02, launcher 0.21.0: second board, Guition 7"; 0.20.1: XInput pads; 0.20: in-game menu; 0.19: online update, Wi-Fi file manager, screenshots, idle dimming; 0.18: folders)

| Item | State | Evidence |
|---|---|---|
| Repo skeleton, board package (SD only), launcher boots | done | flashed on Guition board, COM9 |
| **1.0 DevLink** (info/ls/stat/put/get/rm/mkdir/mv/run/stop/status/log/reboot/crash) + `papp_dev.py deploy` | done | 78 KB upload @ ~205 KB/s; logs + exit events streamed |
| **Spike A** relocatable `.papp` v2 (packer, loader, imports, manifest, CRC) | done | host test: byte-identical at 3 bases; on device apps run at `0x4A000000` and `0x4A030000` |
| SDK runtime (crt0, ctor/dtor, newlib glue, malloc → MEM) | first cut | `libc_test` passes on device (printf/float/qsort/heap/stdio files) |
| Teardown / forced stop (part of Spike C) | first cut | clean stop, forced stop of hung app, leaked blocks reclaimed; heap back to baseline every run |
| Crash reporting (core dump + app context + symbolization) | done | NULL store → `main.c:40` |
| BSP display: ST7701 MIPI-DSI, 2 framebuffers, tear-free flip, vsync, backlight | done | panel upright, colour order confirmed by eye |
| BSP touch: GT911 over shared I2C, polled, points in landscape coordinates | done | crosshair follows the finger (confirmed by eye) |
| BSP audio: ES8311 + I2S0 fixed 48 kHz 16-bit stereo, PA via codec | done | chime heard; 350 ms written in 290 ms (= minus 60 ms DMA queue) → clock correct |
| **2.3 Compositor** first cut (`lx_comp`): one RGB565 surface, FIT_ASPECT/INTEGER/STRETCH/NONE, one PPA SRM (scale + rotate) per present, tear-free flip, touch in surface coords; launcher screens go through it | first cut | `gfx_demo`: 320x240, 400x240, 256x224 (fit and integer), 160x100 stretch at 60 fps (PPA 3.6–6.1 ms); 800x480 1:1 at 23 fps (app drawing 28 ms + PPA 16 ms, serialised); orientation + letterbox checked by screenshot; heap at baseline after every run |
| **DISP v1** interface + SDK wrappers (`papp_disp_*`), optional import | done | apps built before DISP still load and run |
| **2.4 Audio engine** first cut (`lx_audio`): mixer task (core 1, 5 ms blocks) at the fixed 48 kHz, up to 8 push streams (4–192 kHz, mono/stereo, U8/S16, per-stream ring + gain), linear resampler, clipping, underrun = silence + counter, pause/clear/stats | first cut | `audio_test mix`: 11025 U8 mono + 22050 mono + 32000 + 44100 stereo at once, each measured within 0.01 % of nominal, 0 underruns while fed; `cycles 1000`: 0 failures; blocked `write()` released by STOP in < 0.4 s; heap at baseline after every run |
| **AUDI v1** interface + SDK wrappers (`papp_audio_*`) | done | as above |
| **2.5 Input manager** first cut (`lx_input`): USB HID host (several devices), gamepads in a standard layout for 4 players (PS3/PS4/PS5/generic/PSX-adaptor parsers from the reference; a report-descriptor check tells gamepads from other HID interfaces), boot keyboards with US text, one event queue (flushed per app run), HOME / Select+Start (1 s) → stop app | done | DualShock 3 detected as P1 (`054C:0268`); injected pad/key/text arrive as events in `input_test` ("Hi, P4!" typed correctly); HOME and a 1.5 s chord stop the app, a 0.5 s chord does not; heap steady over repeated runs. Real DS3 buttons show the right names on the status screen (checked by hand) |
| **INPT v1** interface + SDK wrappers (`papp_input_*`); DevLink `INPUT` (inject pad/key, read state) + `papp_dev.py input …` and `deploy --inject` | done | as above |
| DevLink `SHOT` (RLE16, host writes landscape PNG) | done | 800x480 screenshot in 0.4 s; buffer freed afterwards |
| Board buttons: BSP `bsp_buttons_init/read` + `bsp_analog_read` (standard-layout bits); Guition: custom GPIO pad add-on (detect via pull-up on GPIO 29, joystick + A/B on shared ADC2 pins, R1 (GPIO 29, also the detect pin)/L1 (30)/X/Y/Start/Select digital, stuck-pin check) and paddle (GPIO 51); lx_input polls at 5 ms with a 2-sample debounce and merges the pad into player 1; INPT v2 `analog()` (`papp_input_analog`), paddle in DevLink input state | done | pad detected (`caps` 127), no phantom presses at rest, paddle readable (±2/255 noise), USB pad and injection still P1. All buttons checked by hand (L1/R1 were swapped vs. the reference notes, fixed) |
| **THRD v1**: threads (create/join/exit/self, stack size + internal/PSRAM, priority class, core), mutex (+recursive), counting semaphore, queue, per-thread TLS slot, `app_exit` from any thread; all tracked: live threads stopped with the main task, objects freed at exit; blocking waits never hold up teardown. SDK `papp_thread_*`, `papp_mutex_*`, `papp_sem_*`, `papp_queue_*`; with `--libc`: per-thread `errno`/stdio state and real newlib locks (one recursive app mutex); `exit()` from a worker ends the app | done | `thread_test`: 4 threads on both cores count to 200000 under a mutex; 2000-item queue across cores in order; 1000 semaphore round trips 14 us each; per-thread errno + concurrent printf; join timeout + `thread_exit` code; 200 create/join cycles 112 us each with internal heap unchanged; 16-thread limit; `leave` (3 threads incl. blocked ones), `exit` (worker `exit(7)` → 7) and `spin` (spinners on both cores, stopped by HOME) all end with the heap at baseline; gfx/audio tests unchanged. Mutex lock+unlock costs ~4.9 us uncontended (FreeRTOS mutex via the interface) |
| **FAST region** (Spike B, ADR D3): `PAPP_FAST` / `PAPP_FAST_DATA` / `PAPP_FAST_BSS`; FAST linked at `0x10000000`; packer: region-tagged relocations + cross-region `PCREL_HI20/LO12` and `DIFF32` (jump tables); loader: FAST in executable internal SRAM (`MALLOC_CAP_EXEC`, PMP split off, cap `CONFIG_LX_APP_FAST_MAX_KB` = 192) or after SLOW in PSRAM when it doesn't fit; crash report maps FAST PCs back to the ELF | done | host test: hello / libc_test / fast_bench relocated == linked directly at 3 (SLOW, FAST) address pairs, byte for byte. Device (`fast_bench`, best of 3, PSRAM vs FAST, 128 KB L2): interpreter 200k ops 16.9 / 18.2 ms (x0.92, L1-resident); crc32 64 KB with table in FAST 2.69 / 2.25 ms (x1.19); 32 KB of code x200 19.5 / 17.8 ms (x1.09); emulator-like frame (interpreter + 32 KB code + PSRAM streaming): 32 KB/frame x0.98, **128 KB/frame 2.45 / 1.81 ms per frame (x1.35)**, 512 KB/frame x1.10. FAST pays off when other memory traffic evicts code from L2; with a quiet cache PSRAM code is as fast. Fallback (FAST in PSRAM) runs correctly. L2 256 KB: PSRAM-only 128 KB/frame case 1.74 ms/frame, but internal free 490 → 360 KB |
| **UI v1/v2** + content browser (`lx_ui`): `pick_file` (folders, All / Favorites / Recent tabs, live search from a physical or on-screen keyboard, per-ROM menu Run or Resume / Restart / Delete save + favorite toggle; gamepad, keyboard, touch with tap/swipe and auto-repeat; reopens at the last ROM played) `message`, v2 `menu` (choices over the app's dimmed picture); favorites / recents shared in `/sd/launcher/`; saves named `<save_dir>/<rom file><save_ext>` (RetroESP32 convention, its saves show up); draws over a running app via `lx_comp_takeover_begin`; SDK `papp_ui_pick_file`, `papp_ui_message` | done | `browse_test` driven over DevLink with screenshots: 143 NES ROMs listed, search "mario" → 7, favorite star, Run / Resume results with correct save paths, subfolder + back, Delete save, Recent tab filtered to the root, on-screen keyboard typing, HOME while browsing ends the app cleanly; internal heap steady |
| **NES app** (`apps/nes`, first emulator as a PAPP): nofrendo from RetroESP32 (LGPL) with its ESP32 layer replaced by `osd_papp.c` + `main.c` (bridge header because nofrendo's 4-byte `bool` can't meet `stdbool`); browser via `papp_ui_pick_file` (Resume / Restart / Run, saves in `/sd/odroid/data/nes/<rom>.sav`, compatible with RetroESP32 saves); 256x224 surface at 2x integer scale, video thread converts + presents; 32 kHz mono stream paces emulation; P1 = board pad + first USB pad, P2 = second pad, left stick as D-pad; X opens the in-game menu (UI v2 `menu`, drawn over the dimmed game: resume / save / load / restart / delete save / exit), Exit game saves and returns to the browser, backing out of the browser ends the app; state also saved on HOME / stop; CPU/PPU/APU hot paths in FAST (38 KB); battery-RAM files next to the ROM disabled (would overwrite the ROM) | first cut | Super Mario Bros 3 (mapper 4) and 1942 (mapper 0) play: 59.8–59.9 fps on screen, 0 audio underruns over 23 s, emulation 7.5 ms/frame with FAST (8.0 ms all-PSRAM); full flow browser → Favorites → Resume → play → HOME saves and exits, heap back to baseline; a RetroESP32 1942 save resumes mid-game. Sound checked by ear (fine). X menu → Save → Exit → browser → second game in the same run works, heap back to baseline. PS3 pad: a genuine DS3 stays silent after a reset until pressed once (in practice PS = HOME), so waking it stopped the app; now the DS3's wake-up PS press is not HOME (checked by the user: PS wakes the pad, the browser and game respond) |
| **Launcher home screen** (`launcher/main/home.c`, LVGL 9.2, design Option 2 adapted to 800x480): logo, clock + date, SD card (free, apps installed), settings gear; app grid (the last app run first, with the focus) from `/sd/apps/*/*.papp` (manifest name; `icon.png` from the app's folder, decoded once and cached (4 MB LVGL image cache), else a coloured initial tile; `papp_build.py` copies an `icon.png` beside the sources, `deploy` uploads it; NES has a placeholder icon); system bar (CPU, SRAM, PSRAM, chip temperature, pads); settings screen (volume, brightness in NVS; system info); gamepad/keyboard focus moves to the nearest item in the pressed direction, A/Start/Enter starts, B/Esc back; touch. LVGL renders one 800x480 PSRAM frame; `lx_comp_launcher_present` rotates it onto the panel with the PPA and stands aside while an app owns the display. Clock: DevLink `TIME`, sent by `papp_dev.py` on every connect (RTC / NTP later). Old status screen removed | first cut | Screenshots: header, clock 23:0x from the PC, 9 apps, live system bar, settings; gamepad navigation; NES started from the home screen, its browser up, B returns home with NES first, heap steady. The "last used" card of the design was dropped in favour of ordering (user's call). Fixed on the way: app main task registered too late when started from a same-core launcher task (store fault in `papp_rt_init`), and `lx_app_path()` read after the app record was freed. Look checked on the real panel by the user |
| **All RetroESP32 emulators as PAPPs** (`apps/<emu>`, cores copied unchanged): `apps/common/odroid` re-implements the reference's `odroid_*` (display writers per core onto one FIT_ASPECT surface, input from P1 + touch edges, audio stream with volume levels, settings in memory), `app_common`, a FreeRTOS subset on THRD (tasks, queues incl. peek/overwrite, semaphores, mutexes) and ESP-IDF shims (`heap_caps_*`, `esp_timer`, attributes: `IRAM_ATTR` → FAST); `emu_main.c` = browser (`papp_ui_pick_file`) → `RomFilePath` / `StartAction` → the core's `*_run()`; the core's own X menu (save / load / exit) and Y volume overlay as in the reference; one game per app load: Exit game ends the app with `PAPP_EXIT_RELAUNCH` and the App Manager starts it again in its browser (fresh core state; checked on GB: exit → browser on the same ROM → B → home). `emubuild.py` (+ C++ cores: libstdc++ with pthread shims). Apps: gb, gbc, sms, gg, col, pce, a78, spectrum, genesis, snes, lynx, a26, a800, neogeo (+ nes). Icons from the reference artwork in `apps/common/icons`. `apps/build_all.py` builds all | first cut | Every core boots a ROM on the board (screenshots): GB Alleyway, GBC Action Man, SMS Asterix, GG Choplifter III, Coleco BIOS, PCE Vigilantes (60 fps), A7800 Alien Brigade, Spectrum Monty, Genesis Aladdin, SNES ActRaiser, Lynx Battle Wheels, A2600 3-D Genesis, A800 Bruce Lee / Boulder Dash / Choplifter (ATR) + Donkey Kong (XEX), Neo Geo Metal Slug (60 fps with frame skip, audio; sprite cache now leaves room for the 3.5 MB 68K IPC pool). Fixed on the way: Game Gear showed VDP lines 0..143 instead of 24..167 (looked white during intros); menu colours were byte-swapped in the reference (yellow showed magenta) in gb/sms/pce/a78; A800 needs `ESP_PLATFORM` for its PSRAM disk cache; Neo Geo's raw FatFs sector streaming replaced by reads from the open `.ctile` file. Not checked yet: sound by ear and controls by hand for the new cores, PCE 1941 is SuperGrafx (unsupported) |
| **v1 games on the v2 SDK** (`apps/common/v1compat`): the reference's v1 `app_services_t` implemented on the SDK, so Doom, OpenTyrian, Quake and Duke3D build with their own glue (newlib wrap, fds) unchanged; MENU / VOLUME from a button or the screen edges | first cut | Doom demo 31 fps, Quake demo 12.4 fps, OpenTyrian and Duke3D title menus; Duke3D hot code now really in FAST (56 KB, its compat header blanked `IRAM_ATTR`) |
| Toolchain fixes for big ports: packer classifies a pc-relative target by its symbol (not S+A: `&memory - 0xd00000` wrapped into the FAST range → "reloc out of range"); `.ext_ram.bss` mapped into `.bss`; libc `fsync`; browser `PAPP_PICK_EXCLUDE_F` (`exclude` list: neogeo.zip, BIOS.col) and `PAPP_PICK_NO_DIRS_F` (Neo Geo cache folders); UI v3 `volume`; DISP v2 `touch_read(NULL)` in display coordinates; manifest `PAPP_MF_HIDDEN` (test apps behind Settings → Show developer apps) | done | host relocation test passes; all 19 apps rebuilt and re-smoke-tested after the change |
| Not yet | — | input: gamepad mapping DB + first-use mapping UI, mouse, keyboard layouts, rumble, pad hot-plug, battery level (GPIO 53); audio: pull model (THRD now exists), better resampler, launcher volume UI; compositor: async present (app draws while PPA runs), multiple surfaces, L8+palette, overlay layer; THRD v2: event flags, timers, fast user-space mutex (uncontended lock in ns, not us); L2 cache size decision with a real emulator (D4); `PAPP_MEM_EXEC` for dynarecs; CMake SDK; `INPUT` |

Known gaps: internal RAM is executable and writable (PMP split off for FAST, no W^X); PPA scale is quantised to 1/16, so STRETCH can leave a few pixels (160x100 → 800x475); a worker thread at PAPP_PRIO_HIGH spinning on core 1 can starve DevLink (prio 4) while it runs (HOME still stops it); `deploy` doesn't
yet notice a device reboot (it waits for the timeout); MEM `realloc` drops alignment.

---

## Next development plan (2026-11-01)

Sizes: **S** = up to 2 days, **M** = about a week, **L** = 2+ weeks. They are rough until each spike is done.
New services follow the existing rules: interfaces are appended and versioned, apps never touch hardware.

### Key findings

- **USB pads:** `lx_input` already uses the reference's method (VID/PID + report-shape detection, one fixed
  generic layout). It handles pads shaped like the known ones. Anything else gets wrong buttons, and XInput pads
  (most Xbox-style pads) aren't HID, so neither project can see them. "Any pad" needs more than the reference has.
- **Wi-Fi / BLE:** the ESP32-P4 has no radio, and the reference never implemented either. This board has an
  ESP32-C6, confirmed working in spike C0 (below), so Milestone C is no longer blocked.
- **BLE pads:** the C6 is BLE only, so Classic Bluetooth pads (DS3, DS4, DualSense, Switch Pro) can't connect this
  way. BLE HID pads should work (needs confirming per pad).
- **OTA:** `ota_0` / `ota_1` (3 MB each, launcher is 1.06 MB) are already in the partition table, unused.
- **Battery:** only "GPIO 53" is noted. The divider ratio and charge curve are unknown.

### Milestone A — Input and power (no radio needed)

| # | Item | Size | Notes |
|---|---|---|---|
| A1 | HID report descriptor parser: **done, 0.14** | — | `lx_hid_parse.c`, tested by `apps/hid_test` (0 failures). Non-Sony pads with a usable descriptor use it; known pads keep their parsers. **No real non-Sony pad tested yet** |
| A2 | Button mapping screen: **done, 0.14** | — | Settings → Map controller, saved per VID:PID in NVS; since 0.23 as text on the card (`/sd/config/gamepads_mine.txt`, shared database `gamepads.txt` from `SDcard/config`, downloaded with the app catalogue; tested on the 7": map, boot, database download, boot). Tested end to end with injected presses, including an A/B swap reaching an app. Skipped buttons keep their default; touch Reset clears it |
| A3 | XInput support: **done, 0.20.1** | — | `lx_input_xinput.c`: a second USB host client claims the Xbox 360 vendor interface (FF/5D/01), sends the vendor start request and the player LED command (8BitDo stays silent without the LED), and reads the 20-byte report. Tested with the 8BitDo Ultimate 2C receiver (2DC8:310A): face buttons and d-pad confirmed; sticks, triggers, shoulders and Start/Home follow the standard layout but were not seen pressed. Xbox One pads (GIP) are not supported |
| A4 | Pad hot-plug and player assignment: **code done, 0.14** | — | A pad returns to its own slot by VID:PID. Needs a physical replug test |
| A5 | Diagnose the USB HID task abort | S | Not reproduced, no data yet. The next occurrence leaves a core dump for `papp_dev.py crash` |
| A6 | Battery level: **done, 0.15** | — | `bsp_battery_mv` (GPIO 53, divider and curve from the reference), `BSP_CAP_BATTERY`, system bar. Pin reads 3.92 V. Hidden outside 2.8-4.5 V. Not yet exposed to apps. Calibration against a measured cell is open |

### Milestone B — Files and home screen (no radio needed)

| # | Item | Size | Notes |
|---|---|---|---|
| B1 | Home screen folders: **done, 0.18** | — | Directories in `/sd/apps` without a `.papp`, one level. New, rename, delete (apps return to the main page, other files are kept) and move, by X / Y or a long press. Replaces the All / Emulators / Games / Other filter. **Long press not tested by touch** |
| B2 | File browser: **first cut, 0.17** | — | `apps/files` PAPP: browse all of SD, details, text preview, delete with confirm (default Cancel). Rename, copy and open-with are missing |
| B3 | File associations | S | Manifest list (`.nes`, `.sfc`, …), which B2 and any future library use |
| B4 | Clock: **done, 0.17** | — | `apps/clock` PAPP: clock, stopwatch, timer. SYS v2 `papp_local_time`. 2.0 (launcher 0.27): round function buttons, close button, alarms (ring while the Clock is open), stopwatch laps, timer ring, world clock (SYS v3 `papp_utc_time` + POSIX zones in newlib) |
| B5 | Sleep and power: **dimming done, 0.19** | S | The home screen dims to 15% after 3 minutes and any input restores it. Not seen on the panel yet. Sleep and wake are not done |

**Clock: both, split by job.** Time itself belongs to the launcher: the home screen needs it, and NTP, time zone
and any RTC must not depend on an app. A full Clock (alarm, timer, stopwatch, big-screen face) should be a
**PAPP**: it needs nothing special, updates independently, and proves the SDK on a real non-game app. Alarms
would only ring while it runs unless the launcher later gets an alarm service. Without NTP or a backed-up RTC
the clock is lost at power-off, so check whether the board has an RTC.

### Milestone C — Radio

| # | Item | Size | Notes |
|---|---|---|---|
| C0 | **Spike: radio bring-up: done 2026-11-01** | — | See results below |
| C0b | Match host and C6 firmware: **done** | — | `esp_hosted` pinned to ~2.0 (2.0.17, same as the C6): mismatch gone. The C6 reports BLE, so C4 needs no C6 update |
| C1 | Wi-Fi service + Settings: **done, 0.16, fixed 0.19** | — | `components/lx_net`. Joining and rejoining a saved network work on the board. A saved network now keeps retrying, a scan works during a join, and DNS has 1.1.1.1 as backup. `NETGEAR98` is -81 dBm here, and joins took 9 to over 90 s |
| C2 | NTP + time zone: **done, 0.16, confirmed 2026-11-02** | — | Zone picker in the Wi-Fi screen. NTP sets the clock once Wi-Fi connects (confirmed on the board by hand). DevLink also sets the clock from the PC |
| C3 | NET interface for apps | M–L | Optional sockets for PAPPs, tracked for teardown. Only when an app needs it |
| C4 | BLE HID controllers | L | NimBLE on the P4 through the C6, pairing screen, feeds `lx_input` as another pad source. Reuses A1 and A2, so it comes after them |
| C5 | Radio during games: **interim, 0.16** | S | Stopped while an app runs and resumed after (tested with the radio up, not connected). Measure the real cost before changing this |

**C0 results** (standalone test project, launcher unchanged):

| Check | Result |
|---|---|
| Wiring | Espressif defaults work: SDIO 4-bit 40 MHz, CLK 18, CMD 19, D0-D3 14-17, reset GPIO 54 (slot 1) |
| C6 | Detected, chip id 0x0d, hosted firmware 2.0.17 |
| Wi-Fi | `esp_wifi_remote` init and start OK, scan found 2 networks |
| SD with radio | Shares the SDMMC host and mounts fine (slot 0). Read speed unchanged: 1,499 KB/s idle, 1,483 KB/s scanning, worst 32 KB block 22 ms either way |
| Not tested | Connecting, real network traffic (needs credentials), BLE, frame-rate impact in games |

### Milestone D — Updates

| # | Item | Size | Notes |
|---|---|---|---|
| D1 | Update from SD: **done, 0.13** | — | `/sd/update/launcher.bin` is offered at boot. Good image tested, survives a reboot. Must be built after this feature, as the version is in the image |
| D2 | App rollback safety: **done, 0.13** | — | Confirmed after 10 s. A build crashing at boot returned to the previous version by itself and the failure was shown once |
| D3 | OTA from GitHub Releases: **done, 0.19.1** | — | Settings → Update reads `latest.json` from the newest release, downloads to the SD card, checks SHA-256 and installs with rollback. Tested on the board from GitHub: 0.19.0 to 0.19.1, twice. `tools/make_release.py` makes the release files. `/sd/update/ota_url.txt` overrides the address |
| D4 | CI release builds | M | GitHub Actions builds the launcher and apps, then publishes a release |
| D5 | Signed updates | M | Verify a signature, not just a hash. Do it before promoting OTA widely |
| D6 | App updates / catalog | M | Same manifest also lists `.papp` files. Respect each core's license when publishing binaries |

### Suggested additions

| Item | Size | Why |
|---|---|---|
| Launcher in-game overlay: **done, 0.20** | — | HOME (or Select+Start held 1 s) pauses the game under a menu: Resume, Volume, Screenshot, Quit to home. Opened by the app's next present, so the game is frozen and its sound held. In a launcher screen (ROM browser) or an app that never presents, HOME quits as before. Tested in NES and Doom. Brightness is not in it yet |
| Wi-Fi file manager (web page): **code done, 0.19** | — | `components/lx_web`, started from the Wi-Fi screen, PIN per start. Only file download has run, over loopback. **The page, upload, delete and new folder are untested** |
| Game library with cover art | L | Browse games across systems, using `.papp` associations (B3) |
| Screenshot button: **done, 0.19** | — | Select + Y saves `/sd/screenshots/shot_NNN.bmp`, tested inside Clock. Can catch a frame mid-update |
| DevLink over Wi-Fi | S–M | Develop without the cable |
| Automated regression run: **first cut, 0.19** | M | `tests/board/smoke.py` runs the relocation test and 4 test apps, and checks for a stored crash. Emulators and forced stop are not covered yet |
| Conformance test app | M | Exercises every interface, for new boards (Phase 8) |
| HDMI board support | M | The reference has it, and it is the first real test of board portability |
| L2 cache size experiment | S | May speed up Quake and others, at a cost in internal RAM |
| ~~Investigate SD speed~~ | done | Fixed in 0.12.0. Cause: stdio buffers aren't DMA-aligned, so SD reads ran one sector at a time. Raw card 18.7 MB/s, `read()` aligned 15.4 MB/s, unaligned 1.6 MB/s. Now each file has a 16 KB aligned buffer: sequential reads 1.5 -> 11.5 MB/s, 256-byte writes 0.28 -> 5.5 MB/s, Quake image read 270 -> 50 ms, Neo Geo load 17 -> 10.7 s. Random 512-byte reads got slower (1.5 -> 3.6 ms), and nothing is known to depend on them |
| Save sync over Wi-Fi | L | Later, once Wi-Fi is stable |

### Suggested order

1. **Now:** finish hands-on testing. D3, C2 and the file manager are confirmed on the board.
2. **Input:** try real pads against A1 and A4, then A3.
3. **Everyday features:** B3, finish B2, overlay.
4. **Radio:** D4, D5, C4.

### Open decisions

- Is there a battery fitted, and which cell?
- Which BLE pads matter most? This decides how much of C4 is worth building.
- Is the radio allowed to run during games?

---

## Overview

| Phase | Title | Est. | Depends on | Can overlap with |
|---|---|---|---|---|
| 0 | Foundations & technical spikes | 2 pw | — | — |
| 1 | BSP layer + bring-up on the reference board | 2–3 pw | 0 | 3 (design part) |
| 2 | Core launcher services | 4 pw | 1 | 4 |
| 3 | ABI v2, loader, App Manager | 3 pw | 0, 2 (partially) | 4 |
| 4 | SDK, toolchain & conformance suite | 3 pw | 3 (headers) | 2, 3, 5 |
| 5 | Launcher UI, overlay, dev tools | 3 pw | 2, 3 | 4, 6 |
| 6 | Port existing PAPPs to the SDK | 3–4 pw | 3, 4 | 5 |
| 7 | Emulators-as-PAPP pilot (decision gate) | 3–5 pw | 6 | 8 |
| 8 | Second board port (portability proof) | 2–3 pw | 6 | 7 |
| 9 | Hardening, ABI freeze, release 1.0 | 2–3 pw | all | — |
| **Total** | | **27–33 pw** | | ≈ **15–18 calendar weeks with 2 devs** |

Suggested split for two developers:
- **Dev A (platform)**: 0 → 1 → 2 → 5 → 8
- **Dev B (ABI/SDK/apps)**: 0 → 3 → 4 → 6 → 7
- Both: 9.

Milestones:

| Milestone | Content | End of |
|---|---|---|
| **M1 "Hello, relocated"** | A relocated `.papp` runs at an arbitrary address on the new launcher skeleton | Phase 0 + 1 |
| **M2 "Services online"** | Compositor, mixer, input, FS, arenas working; loader v2 + App Manager; SDK builds template | Phase 2 + 3 + 4 |
| **M3 "Feature parity"** | New launcher UI runs Doom/Quake/Duke3D/OpenTyrian/LVGL from the SDK, no leaks | Phase 5 + 6 |
| **M4 "Portable"** | Same `.papp` binaries run on a second board; emulator decision made | Phase 7 + 8 |
| **M5 "1.0"** | ABI v2 frozen, docs, release images | Phase 9 |

---

## Phase 0 — Foundations & technical spikes (2 pw)

Goal: remove the big unknowns before writing production code. Each spike ends with a short ADR in
`docs/adr/` (decision, measurements, consequences).

**0.1 Repository & conventions** (2 d)
- Git repo layout: `launcher/` (ESP-IDF project), `boards/`, `components/` (launcher core
  components), `papp-sdk/`, `apps/`, `tools/`, `docs/`, `SDcard/` (staging mirror, as in reference).
- `.clang-format`, naming conventions (`papp_*` = ABI, `lx_*` = launcher internals, `bsp_*` = board).
- Build scripts (PowerShell + Python), reuse `Resolve-IdfEnv.ps1` from the reference.
- CI (GitHub Actions, `espressif/idf:v5.5.6` container): build launcher for every board config, build
  SDK + all apps, run host unit tests of `papp-pack`.

**0.2 Spike A — relocatable images** (3 d)
- Link the reference template with `-Wl,-q` at base 0; write a prototype `papp-pack` that extracts
  `R_RISCV_32` relocs; prototype loader applies them at the address `esp_mmu_map()` returns.
- Test: same binary loaded twice at two different addresses in one session; a test app with function-
  pointer tables, string-pointer arrays and `.init_array`.
- Exit: works; list of relocation types that appear in real code (run the packer over the Quake and
  Doom ELFs from the reference to see the full set).

**0.3 Spike B — executable internal SRAM + performance** (3 d)
- Option (a): `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n`, allocate an arena, execute from it.
  Option (b): keep split; reserve a region in the launcher linker script and open it with a PMP entry.
- Benchmark a CPU-bound kernel (e.g. a 6502/Z80 interpreter loop from nofrendo/smsplus, plus a
  memcpy/blit loop) from: flash (IDF build), IRAM, PSRAM-XIP, FAST arena; L2 cache 128 KB vs 256 KB.
- Exit: ADR D3/D4 with numbers; decision on default FAST arena size.

**0.4 Spike C — forced teardown** (2 d)
- App with 3 threads (one blocked on a queue, one spinning, one in `delay`) + allocations + open files;
  launcher kills and reclaims; repeat 500×; check heap returns to baseline.
- Exit: ADR D5; list of FreeRTOS gotchas (deleting tasks holding mutexes, static TCB/stack ownership).

**0.5 Spike D — libc in apps** (2 d)
- Link toolchain newlib + `-lstdc++` (minimal) into a test app; implement retarget locks via a mutex
  service; verify `printf`, `fopen`, `qsort`, `strtod`, `new/delete`, static constructors.
- Exit: ADR D6; known-good link line for the SDK.

---

## Phase 1 — BSP layer + bring-up on the reference board (2–3 pw)

**1.0 DevLink service** (3 d, done first — it accelerates everything after it) — framed protocol over
USB-Serial-JTAG, log routing, file commands (`LS/PUT/GET/RM/MKDIR`), `RUN/STOP/STATUS`, `INFO`,
`REBOOT`, crash record; host tool `tools/papp_dev.py` with `deploy`. `SHOT` and `INPUT` are added in
Phase 2 once the compositor and input manager exist. See ARCHITECTURE §7.8.

**1.1 Define `bsp_*.h`** (2 d) — display, backlight, touch, audio, buttons/analog, SD, USB host
power, battery, capability descriptor (see ARCHITECTURE §7.1). Review together with Phase 3 owner.

**1.2 Board package `boards/guition_p4_43`** (5 d) — port from the reference project:
- `st7701_lcd` + MIPI-DSI init → `bsp_display_*` with 2–3 DMA framebuffers in PSRAM and vsync callback.
- `gt911_touch` → raw points + transform descriptor.
- ES8311/I2S from `odroid_audio` → `bsp_audio_*` (fixed rate).
- GPIO pad incl. per-pin stuck detection (`odroid_input.c`), paddle ADC.
- SDMMC mount with FATFS LFN config.
- `sdkconfig.defaults` per board (PSRAM HEX 200 MHz, cache, FATFS LFN, USB host).

**1.3 Board package `boards/guition_p4_hdmi`** (3 d) — LT8912 bridge + shared I2C (reference
`HDMIport.md`), no touch/paddle caps. Removes every `CONFIG_HDMI_OUTPUT` `#ifdef` from core code.

**1.4 Built-in hardware self-test** (2 d) — launcher boot option that shows colour bars + touch
crosshair + button matrix + tone sweep + SD speed test. Reused for every future board port.

Exit criteria: self-test passes on LCD and HDMI; boot to self-test < 1.5 s.

---

## Phase 2 — Core launcher services (4 pw)

Each service is an ESP-IDF component in `components/lx_<name>/`, independent of the loader, with an
on-device test runner (Unity) so it can be validated before any app exists.

**2.1 Memory arenas** (3 d) — app PSRAM arena + FAST arena on `multi_heap`/TLSF; abstract caps
(`FAST/DMA/LARGE`), aligned allocations (64 B for DMA/PPA), stats, high-water marks, leak report.

**2.2 Threads & sync with tracking** (3 d) — wrappers for task (internal or PSRAM stack, like the
reference `svc_task_create`), mutex, recursive mutex, semaphore, queue, event flags, software timers;
priority classes; resource lists per app context.

**2.3 Compositor** (6 d)
- Surfaces (RGB565, RGB888, L8+palette), 1–3 buffers, scale modes (FIT_ASPECT/INTEGER/STRETCH/NONE),
  transform from BSP panel orientation; one PPA SRM per present; letterbox clear only on change.
- Overlay layer (ARGB) blended with PPA; vsync-paced flip; frame stats (FPS, present latency).
- L8→RGB565 converter (PIE SIMD path + C fallback).
- Touch inverse-transform into surface coordinates.
- Test: 320×200, 320×240, 400×240, 256×224, 640×480, 800×480 surfaces on LCD and HDMI.

**2.4 Audio engine** (5 d) — fixed-rate I2S writer task (core 1, high prio, internal DMA buffers),
N streams with ring buffers, linear (later polyphase) resampler, mixer, master volume/mute, push +
pull models, underrun counters. Test: 11 025/22 050/32 000/44 100 Hz streams simultaneously; 1 000
open/close cycles with no glitch or deadlock.

**2.5 Input manager** (5 d) — port `usb_host_hid` + `gamepad` (incl. auto-mapping DB), add HID
keyboard (boot + report protocol) and mouse; board buttons + paddle; logical gamepads (4 players,
standard layout + axes); event queue; system chord detection (HOME/MENU → launcher).

**2.6 Storage** (2 d) — tracked file/dir handles, sandbox path helpers (`app_dir`, `data_dir`),
`stat` with real sizes, rename/remove/mkdir.

**2.7 KV settings** (1 d) — per-app store (`/sd/data/<id>/settings.kv` or NVS namespace hash) + global prefs.

**2.8 Gfx / image services** (3 d) — generalise reference `sprite_blit`/`fb_copy`: PPA SRM/blend/fill
jobs (sync + async), 2D-DMA/async memcpy, PNG decode (pngdec), HW JPEG decode. No board-specific
colour swaps.

Exit criteria: all service unit tests green on both board configs; audio + compositor run 1 h soak.

---

## Phase 3 — ABI v2, loader, App Manager (3 pw)

**3.1 ABI headers** (3 d) — `papp_abi.h` (header, manifest TLVs, import entries, reloc encoding,
interface IDs/version rules) + one header per interface. Design review with both devs; mark as
`v2-preview`.

**3.2 Loader v2** (5 d) — header/manifest parse, compatibility check before allocation, SLOW + FAST
region allocation, streaming read, relocation (ABS32, cross-region PCREL/CALL pairs), import
resolution, cache maintenance + exec mapping, CRC verification. Load-time profiling (target: 1 MB
image < 300 ms from SD).

**3.3 App Manager** (4 d) — app context, `papp_main` task creation from manifest, lifecycle events
(PAUSE/RESUME/EXIT_REQUEST), normal and forced teardown, heap-baseline check, argv passing.

**3.4 Crash reporting** (2 d) — panic hook → `.noinit` crash record → post-reboot report screen +
`/sd/launcher/crash/*.txt`; optional task watchdog for `papp_main`.

Exit criteria: conformance stubs load at arbitrary addresses; **1 000× load → run → exit loop with
zero heap delta**; forced quit works for an app stuck in an infinite loop.

---

## Phase 4 — SDK, toolchain & conformance suite (3 pw)

**4.1 Runtime library** (5 d) — `crt0`, constructors/destructors, newlib syscalls (single shared
implementation replacing the four `papp_syscalls.c`), malloc routing policy, retarget locks, minimal
C++ runtime, generated interface stubs + import table, `PAPP_MANIFEST()` macro, `PAPP_FAST` attributes.

**4.2 Build system** (3 d) — CMake toolchain file + `papp_add_app()` function; also a PowerShell
wrapper for people who don't want CMake. Linker script with SLOW/FAST output sections.

**4.3 Tools** (4 d) — `papp-pack` (validation: unsupported relocs, forbidden sections, missing
manifest, size report per region), `papp-upload` / `papp-run` / `papp-log` (extends reference `PAPU`
serial protocol), `papp-symbolize` (crash address → function:line).

**4.4 Templates & docs** (2 d) — hello, sprite demo (port of `ESP32_P4_PAPP_Template`), C++ app, LVGL
app; `docs/SDK_GUIDE.md` replacing `PAPP_GUIDE.md`.

**4.5 ABI conformance suite** (3 d) — one PAPP that exercises every function of every interface
(including error paths and optional-interface absence) and prints a PASS/FAIL table over serial and on
screen. **This is the acceptance test for every launcher release and every board port.**

Exit criteria: one-command build of all templates on Windows and in CI; conformance suite 100% on
both board configs.

---

## Phase 5 — Launcher UI, overlay, dev tools (3 pw)

**5.1 LVGL integration** (2 d) — LVGL v9 on the compositor (launcher draws into its own surface; UI
torn down/parked while an app runs).

**5.2 Home screen** (4 d) — app grid/carousel, icons, details page, index cache with fast rescan,
greyed-out incompatible apps with the reason ("needs touch", "needs AUDIO v3").

**5.3 File browser + associations** (3 d) — browse `/sd/roms/...`, filters from manifests, favourites
and recents (carry over the reference concepts), launch with argv.

**5.4 Overlay menu** (3 d) — HOME chord → pause app → menu (resume, volume, brightness, app items,
screenshot, quit); volume/brightness OSD; UI dialogs for the `UI` interface (file picker, message box,
on-screen keyboard).

**5.5 Settings & gamepad mapping UI** (2 d).

**5.6 Dev mode** (1 d) — serial upload/run/log/crash fetch while in the launcher.

Exit criteria: full navigation with gamepad, touch and keyboard; boot-to-home < 2 s with 50 apps.

---

## Phase 6 — Port existing PAPPs to the SDK (3–4 pw)

Order chosen from simplest to hardest; each port deletes its private libc glue and watchdog/exit code.

| App | Work | Est. |
|---|---|---|
| Template + touch test | rewrite on SDK | 1 d |
| LVGL demo | display/indev/tick on SDK, drop `papp_libc.c` | 2 d |
| **sdl2-lite** library | SDL video/audio/events/timer subset on ABI (from OpenTyrian/Duke3D shims) | 4 d |
| OpenTyrian | on sdl2-lite | 2 d |
| Duke3D | on sdl2-lite | 3 d |
| Doom (prboom-go) | `rg_*` stubs → ABI; exit via lifecycle event instead of longjmp watchdog | 3 d |
| Quake | same; 256 KB stack via manifest; hunk in LARGE arena | 3 d |

Perf check: FPS and load time vs the reference launcher for each game; profile hot functions and
tag them `PAPP_FAST` where it pays off.

Exit criteria: all apps run, quit via overlay, **100 consecutive launches without reboot or heap
growth**, FPS ≥ reference.

---

## Phase 7 — Emulators-as-PAPP pilot (3–5 pw, decision gate)

Goal: find out whether the OTA/reboot model can be retired.

1. **NES (nofrendo)** first — small, well understood (1 pw). Use `argv[1]` ROM path, push audio
   stream, L8 surface, `PAPP_FAST` on the CPU/PPU core.
2. **SNES (snes9x) or Genesis (gwenesis)** — the most demanding 60 FPS cores (2–3 pw). Port the
   reference frame pacer and per-emulator audio strategy; profile; move hot code into FAST.
3. **Gate review**: if ≥ parity (60 FPS, no audio crackle) → plan migration of the remaining cores
   (separate plan, ~1–2 pw each). If not → keep an "OTA native firmware" app type in the launcher
   (manifest type `firmware`, launcher flashes/boots it) for the heavy cores and revisit later.

Save states map to the overlay's app menu items and `/sd/data/<id>/saves/`.

---

## Phase 8 — Second board port (2–3 pw)

**Status 2026-11-02 (0.21.0): first port done.** `boards/guition_p4_7` (Guition JC1060P470C, 7" 1024x600
JD9165 landscape, GT911, ES8311 + NS4150). The same SD card runs unchanged: NES, Doom, Quake, Clock,
the in-game menu and `smoke.py` all pass. Launcher fixes it needed: home, Settings, Wi-Fi and Map
screens follow the screen size; esp_hosted pinned per board (the C6 firmware differs); the image's
project name carries the board, so updates (SD and online) never cross boards; `latest.json` has a
per-board section. `docs/PORTING_GUIDE.md` written. The 7" board's RX8025T RTC keeps the time across power-off (`bsp_rtc_*`, set from NTP); on boards with an RTC, the home screen clock opens a date and time setup (time zone, fields, sync from internet). Not done: the conformance app,
the V2 panel table, and touch and sound checked by hand.

**Third board, M5Stack Tab5 (`boards/m5stack_tab5`):** ILI9881C 720x1280 panel used in landscape,
GT911, ES8388, RX8130 RTC, power rails on two I2C expanders, C6 with esp_hosted 1.4 (M5Stack's C6
firmware). Same SD card: NES, Doom, Wi-Fi scan and `smoke.py` pass. Added on the way: `papp_dev.py
bootlog` (log kept from boot). Orientation, touch and sound checked by hand. Not done: the battery
(2 cells via INA226). The ST7123 / ST7121 panel variants are detected and set up as in M5Stack's BSP,
waiting for a test on such a unit.

Pick a board that is **meaningfully different** from the reference to stress the abstraction — e.g.
a 7″ 1024×600 landscape P4 board (Espressif ESP32-P4-Function-EV-Board or a Waveshare ESP32-P4
board), different codec, no GPIO pad (USB-only input).

1. New `boards/<id>/` package (wrap the vendor BSP where one exists) — 1–1.5 pw.
2. Run self-test + conformance suite — fix *launcher core* bugs found (expected!).
3. Copy the SD card from the reference board **unchanged**: every `.papp` must run byte-identical.
4. Write `docs/PORTING_GUIDE.md` from the experience (checklist, BSP contract, test procedure).

Exit criteria: conformance 100%; all Phase 6 apps run unmodified; porting guide reviewed.

---

## Phase 9 — Hardening, ABI freeze, release 1.0 (2–3 pw)

- Soak tests: 24 h app cycling; SD removal while running; low-memory apps; power-loss during save.
- Fault injection: apps that crash, hang, leak, spam logs, open 1 000 files → launcher always recovers.
- Load-time and boot-time optimisation.
- **Freeze ABI v2** (remove `preview`), publish versioning policy (interface minor/major rules).
- Launcher self-update from SD (A/B OTA).
- Release packaging: merged firmware per board, SD-card image, SDK zip, docs (User guide, SDK guide,
  Porting guide, Architecture).

---

## Backlog (post-1.0)

- `NET` interface: Wi-Fi via esp_hosted (P4 boards with a C6 companion); Wi-Fi upload in dev mode.
  App download is done in the launcher (0.22.0): each app has a `version.txt` (PAPP_APP_VERSION),
  `tools/make_apps_release.py --publish` puts the catalogue on the "apps" pre-release, and Settings → Apps (the Store since 0.26)
  installs or updates apps (SHA-256 checked, updated in place). Same release: one launcher-drawn game menu
  for every emulator (`odroid_menu.h`), and the launcher volume replaces the cores' own volume levels.
- `BTHD`: BLE HID gamepads/keyboards.
- `USBD`: USB MSC mode to expose the SD card to a PC.
- **PC simulator**: Windows/Linux host that implements the ABI on SDL2 and loads apps built as
  native DLLs — fast iteration without hardware.
- `MEM_EXEC` for dynarec/JIT emulators.
- `CAM`: MIPI-CSI camera + HW JPEG/H.264 encode.
- Multi-language launcher UI, themes.

---

## Immediate next steps (week 1)

1. Agree on this architecture (especially D1, D2, D6 in ARCHITECTURE §11) and the board ID naming.
2. Create the repository skeleton + CI (0.1).
3. Start Spikes A and B in parallel — they decide the binary format and whether emulators can follow.
4. Order the second board for Phase 8 now, so it's on the desk when needed.

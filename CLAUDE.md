# ESP32-P4 Launcher — Project Context

Launcher firmware for ESP32-P4 boards that loads **relocatable PAPP v2 apps** from the SD card into
PSRAM and runs them with a DLL-style interface layer. Successor to the PAPP concept in
`C:\ESPIDFprojects\RetroESP32_P4` (reference project: drivers, ports, history).

Design: `docs/ARCHITECTURE.md`. Plan + status: `docs/WORKPLAN.md`. Which boards qualify (supported / community / retired): `docs/BOARD_SUPPORT.md`.

## Layout

```
launcher/                 ESP-IDF project (factory app; main.c + home.c = LVGL home screen + app folders + app search
                          (any folder, USB keyboard letter opens it) + red dot on the gear when an online update waits (quiet check, 12 h)
                          + start at boot (Settings "At boot" = search screen in pick mode, /sd/config/autostart.txt; 2.5 s
                          "Starting" screen, held touch/button/key = Quit or Start box; skipped after a crash or panic/WDT reset); store.c = the Store,
                          first home tile: categories, app pages, install/update/remove; home_ui.h = what it uses from home.c). Board: -DLX_BOARD=<id> (default guition_p4_43)
boards/<id>/board/        BSP implementation (component "board", pins esp_hosted for its C6); boards/<id>/sdkconfig.board.
                          guition_p4_43 (4.3" 480x800, image "launcher"; guition_p4_43_v3 = same sources built for
                          rev v3.x chips, BOARD_CHIP_V3 picks the id), guition_p4_7 (7" 1024x600; guition_p4_7_v3 = v3.x chip, which comes with
                          the 2026 "New Panel" (rear label V2; some v1.x boards have it too: guition_p4_7 detects it at a power-on, kept in RTC memory
                          over restarts): other JD9165 table/timing (Guition's V2 demo: RGB565, 52 MHz) + COLMOD 0x55 (without it faint
                          vertical lines), reset GPIO 0; such boards are the newer JC1060P470C_I_W_Y: speaker amp on GPIO 11, not 20 (board_is_y),
                          C6 SDIO 10 MHz on both 7" images), m5stack_tab5 (720x1280,
                          power rails on two PI4IOE5V6408 I2C expanders, esp_hosted 2.12.13 (Tab5 preset, C6 reset active high;
                          runs M5Stack's 1.4.1 C6 firmware too, Settings > Update offers 2.12.13, #48); INA226 battery + charging (board_power.c,
                          PWR interface); clip-on Tab5 Keyboard at 0x6D on I2C port 0, G0/G1 (board_keyboard.c, BSP_CAP_KEYBOARD);
                          BMI270 accelerometer = PAPP_ANALOG_ACCEL_X/Y/Z 16..18 via bsp_analog_read, started on an app's first read, board_imu.c),
                          waveshare_p4_rpi_dsi (RETIRED after 0.30.0, make_release.py RETIRED; example port: Waveshare dev kit + RPi Touch Display V1-style 800x480 panel: TC358762, ATTINY88
                          power/backlight at 0x45, FT5x06 touch; RGB565 shadow buffers converted to the RGB888 scan-out
                          by the PPA, docs/PORT_WAVESHARE_P4.md), elecrow_p4_aio (Elecrow All-in-one Starter Kit SEE00804D:
                          7" 1024x600 EK79007, scan direction on GPIO 32/33 = rotation 0, I2S amp without codec (software
                          volume), 1-bit SD, ADC button ladder, C6 over SPI full duplex (no release C6 image: that one is SDIO); untested,
                          docs/PORT_ELECROW_AIO.md)
components/bsp/           BSP contract (bsp.h) — the core only talks to hardware through it
components/papp_abi/      papp_abi.h — .papp v2 format + interfaces (shared with apps; no IDF includes)
components/lx_loader/     App Manager: loader, relocation, imports, services (SYS/MEM/FS/DISP/AUDI/INPT/THRD/UI/NET/BLE), teardown;
                          MEM v2 code_alloc/code_free/code_sync = executable PSRAM for JITs (64 KB blocks mapped a second time
                          with EXEC; esp_mmu_map runs on an internal-stack helper task, it disables the caches);
                          lx_lpram.c = the P4's LP SRAM (0x5010xxxx, ~50x slower, IDF puts it in the heap as internal RAM): taken
                          out of the heap at boot and before each app (IDF's start-up holds some: on the Guition boards the main
                          task's stack), kept as private heaps for MEM v3 PAPP_MEM_LPRAM only (#38); never FAST memory or a stack;
                          NET (lx_svc_net.c) = Wi-Fi, sockets, TLS, HTTP(S) for apps: the first call holds the radio
                          for the run (lx_net_app_hold), every wait gives up on exit, teardown closes sockets and rejoins the saved network;
                          BLE = Bluedroid (dual mode) on the Guition boards, Tab5, Elecrow and waveshare_p4_rpi_dsi (CONFIG_BT_* in their
                          sdkconfig.board): lx_svc_ble_bd.c on the shared core lx_bt.c (stack started/stopped as jobs on the "lx_bt"
                          task, internal stack; users = app + speaker; controller = a USB Bluetooth dongle when plugged in and LE-capable,
                          else the C6 over esp_hosted VHCI (LE only; lx_bt.c fakes the C6's missing 0x1005/0x1004 replies); bonds in
                          Bluedroid's NVS store). lx_bt_usb.c = USB HCI transport (class E0/01/01: commands as control transfers,
                          events on interrupt IN, ACL on bulk); also the project's own ESP32 dongle (giltal/ESP32_BT_Dongle,
                          classic ESP32, controller-only, H4 on UART0) behind its CH34x/CP210x: 921600 8N1 set with the chip's
                          vendor requests, DTR/RTS off, H4 over the bulk endpoints (h4_feed), taken only after identify 0xFC7E
                          answers "LXBT" (other serial gadgets left alone); speaker + DualShock 4 together work; a dongle without LE (Bluetooth 1.x-3.0) gets LE answered by the transport
                          (Classic only; an old genuine CSR also gets its unknown start-up commands answered), answers features page 1
                          for counterfeit CSR8510s (0A12:0001, last page 0), board = master of every accepted link; the counterfeit
                          CSR8510 can't run a speaker + Classic pad together (an old genuine CSR does); Realtek dongles need a firmware
                          download that isn't done. lx_bt_pads.c = Bluetooth gamepads on the dongle: Classic HID host +
                          own HID-over-GATT client for BLE pads (/sd/config/bt_pads.txt; same Settings screen); lx_bt.c fans
                          Bluedroid's single GAP BLE / GATTC callbacks out to slots. DevLink `bt` (papp_dev.py bt status|scan|
                          pair|speaker..) = test aid; -DLX_BT_TESTPAD=1 build = board poses as a BLE pad (lx_bt_testpad.c).
                          lx_bt_speaker.c = A2DP source to a speaker/headphones: home Settings "Speaker"
                          screen (scan, pick, connect, forget), /sd/config/bt_speaker.txt, calls after 10/20/40 s then every 60 s
                          (with a pad connected: 2.5 s pages every 30/60/120 s, only after 20 s without pad input: a page
                          stops all LE data on the CSR; pads are never cut off); lx_audio_set_tap sends the
                          mixer's output there (resampled 48 kHz -> A2DP rate, master volume in software) and silence to the codec.
                          NimBLE (lx_svc_ble.c) remains for boards that pick it: host starts on the first call and stops at teardown,
                          a C6 on 1.x firmware (stock Tab5) runs its controller from boot; bonds in /sd/config/ble_bonds.bin.
                          The slot is NULL on the Waveshare 3.5"; BLE v2 = pairing;
                          FS v2 = stat/rename/rmdir/opendir (closed at teardown); SYS v5 = random; USBH (lx_svc_usbh.c) = raw USB host
                          client; PWR (lx_svc_pwr.c) = battery, charging back on at teardown; HIDD (components/lx_usbd, TinyUSB pinned) =
                          USB-C as the app's HID device: DevLink paused, the FS OTG takes PHY 0; a boot after a crash with it taken
                          resets the USB units (RTC mark); IRQ (lx_svc_irq.c) = free header pins (bsp_app_gpio_mask per board) +
                          gptimers, interrupts on the app's core: GPIO line 1 = core 0, line 2 = core 1 (IDF's ISR service has line 0),
                          allocated/freed on a task pinned to the core; deferred (event ring, wait) or direct (app's PAPP_FAST code in
                          the ISR, FAST must be in SRAM); PAPP_IRQ_AWAKE = priority-1 spinner on the core (an idle core on a v1.x chip takes
                          ~17 us longer to enter an ISR; not on v3.x); ARCHITECTURE §7.9;
                          CODC (lx_svc_codec.c) = decoders (open/send/receive_frame|receive_audio/flush/close, cores 1|2): MP3 + AAC
                          built in (esp_audio_codec 2.5.0, ~200 KB), H.264 = codec module /sd/launcher/codecs/h264.papp (apps/codecs/h264:
                          FFmpeg 7.1.1, LGPL; papp_build --module, module_crt0.c, libc_glue -DPAPP_MODULE = own reent per thread),
                          loaded by lx_module_load (lx_app.c) on the first open, unloaded at teardown; + hardware JPEG info/decode/
                          encode in RGB565 (bounce buffers, engines per run; decode needs sizes that are multiples of 8); DISP v5 blit_yuv = I420 frame -> PPA YUV420 (packed
                          "U Y Y / V Y Y" lines, repacked by the CPU) -> RGB565 scaled into the surface (lx_comp_blit_yuv); §7.11
components/fatfs/         copy of IDF 5.5.6's fatfs with exFAT on (FF_FS_EXFAT 1, + FF_USE_LABEL fix): overrides IDF's;
                          diskio_sdmmc.c: a 4-bit SD transfer that times out moves the card to 1-bit (CMD12 + ACMD6, no
                          re-init or controller reset: the C6 shares it) until reboot (v3.2 7" with bad D1-D3);
                          ff_sdmmc_bus_width() / ff_sdmmc_bus_fell_back() = home SD panel "SD Card (4-bit)" (orange
                          "(1-bit)" after a fallback), Settings "(1-bit, 4-bit failed)", DevLink info sd_bits / sd_1bit;
                          README_LAUNCHER.md = the changes, to redo when moving to another IDF version
components/lx_devlink/    DevLink: framed protocol over USB-Serial-JTAG (devlink_proto.h)
components/lx_common/     crc32, launcher version; lx_zip = zipped ROMs (central directory + the chip ROM's miniz tinfl):
                          an app opening "dir/X.nes" that isn't there gets the .nes member of "dir/X.zip" (lx_services.c
                          fs_open/fs_size): unpacked into PSRAM behind fopencookie, or for .gba/too big into
                          /sd/launcher/zipcache (latest only, source.txt = what it holds); lx_ui's browser lists .zip for
                          emulators whose extensions lack it and hands over the virtual name (saves follow it, recents keep the .zip)
components/lx_gfx/        software drawing + 8x16 font for launcher screens (rotation-aware)
components/lx_comp/       compositor: app surfaces → PPA scale/rotate → panel; launcher screens draw via it;
                          present is async with 2 buffers (task "lx_present" scales + flips while the app runs;
                          present_ext copies the app's frame into the spare buffer first), synchronous with 1 buffer
components/lx_audio/      audio engine: per-stream rings → resample → mix → BSP codec (fixed rate)
components/lx_ui/         launcher-drawn screens for apps: content browser (pick_file; <root>/gamenames.txt "mslug=Metal Slug" = the
                          names shown, sorted and searched: the Neo Geo set, SDcard/roms/neogeo, downloaded by the Store) + message box; UI v4 wifi() = Wi-Fi picker
                          (scan list, password keyboard, joins with lx_net_connect = saved to wifi.txt; lx_net_restore on fail/cancel)
components/lx_input/      input manager: USB HID gamepads/keyboards, XInput (Xbox 360) pads (lx_input_xinput) + board buttons (merged into P1) → players + event queue; HOME chord;
                          USB hubs (CONFIG_USB_HOST_HUBS_SUPPORTED, every board): no TT in IDF, so lx_input_usb.c wraps hcd_port_command
                          to keep the root port full speed (FSLS only, 30 MHz clock, frint 29999, UTMI fc_06 FS EOP + LS preamble bits);
                          lx_hid_parse = HID descriptor parser; lx_padmap = per-pad button mapping as text on the card:
                          /sd/config/gamepads_mine.txt (Map controller) wins over /sd/config/gamepads.txt (shared database,
                          SDcard/config/gamepads.txt, downloaded by the Store); NVS "padmap" = pre-0.23, read-only fallback
components/lx_update/     self-update from /sd/update/launcher.bin + rollback confirm; online update (latest.json);
                          lx_appstore = app catalogue for the Store (apps.json on the "apps" release; icons cached in /sd/launcher/store;
                          codec modules: an app's "needs" installs them in /sd/launcher/codecs, a fetch updates the ones there)
components/lx_net/        Wi-Fi station, NTP, time zone via the ESP32-C6 (esp_hosted 2.12.13 on the Guition boards, Tab5 and waveshare_p4_rpi_dsi, 1.4 on the Waveshare 3.5");
                          lx_c6 = the C6's firmware: version (DevLink info c6_fw) and update from /sd/update/c6.bin or the release;
                          saved network = /sd/config/wifi.txt (ssid=/password= lines; NVS "net" = pre-0.24, moved to the card at boot)
                          join = all-channel scan, strongest AP of the SSID (mesh/repeaters share one); no address 20 s after joining = rejoin
components/lx_web/        Wi-Fi file manager: web page for the SD card, on request, with a PIN per start
papp-sdk/                 App SDK: include/papp/papp.h, runtime/ (crt0, libc glue), ld/papp.ld,
                          tools/papp_build.py, tools/papp_pack.py. App version: apps/<app>/version.txt
                          (beside the first source or the build.py) -> PAPP_APP_VERSION in the manifest
apps/                     Test/demo apps (hello, libc_test, gfx_demo: DISP test, args w h mode secs bufs;
                          audio_test: AUDI test, args mix|cycles|block; input_test: INPT event log, arg secs;
                          thread_test: THRD test (--libc), args all|leave|exit|spin;
                          fast_bench: PSRAM vs FAST (internal SRAM) placement benchmark, arg rounds;
                          browse_test: UI browser test, args root exts save_dir picks;
                          net_test: NET test (--libc), args all|serve|ap|hang|join ssid pass [url]|fetch url [method] [body];
                          ble_test: BLE test (--libc), args periph [secs]|scan [secs]|central [name] [secs]|hang;
                          periph = "gution-p4" for ESP32P4_Gution4_7project/tools/ble_client.py;
                          ppa_test: DISP v4 blit/fill self-check against a CPU model (--libc), exit code = failures;
                          irq_test: IRQ test (--libc), args all|leave|hang [pin]: toggles a free pin (no wiring, it sees its own
                          edges); codec_test: blit_yuv vs a CPU model + JPEG round trips and timings + the decoders on /sd/codec_test
                          media (make_media.py, transcode.ps1 = Windows Media Foundation encodes MP3/M4A/MP4, mp4_to_packets.py)
                          (--libc), arg secs, in the Store under Tools — the other test apps are
                          PAPP_MF_HIDDEN: shown on the home screen only with Settings → Show developer apps)
apps/education/<app>/     Education apps for students (ids edu.*): `python apps/education/build.py` (build_all runs it too)
                          builds them into build/education and copies .papp + icon to SDcard/apps/Education/<app>/ (committed;
                          the "Education" home screen folder, icon from apps/common/icons/folder_education.png). Also in the Store
                          (category education, folder Education; make_apps_release reads build/education too). common/edu_ui.c (linked into each):
                          touch UI on an 800x480 surface, log panel, the remotes' shared state + Control screen.
                          ble_remote = BLE Remote, the Gution BLE demo ("gution-p4": control service mode/level/led/msg + Nordic
                          UART console, same protocol as ESP32P4_Gution4_7project's GutionTerminal); wifi_remote = Wi-Fi Remote,
                          web server on port 80 (page, GET/POST /state in the Gution JSON, GET /set?level=..), hotspot
                          "P4-Remote-xxxx" (password p4remote) when there is no network or on the Hotspot button; Network button /
                          "Choose network" = the launcher's Wi-Fi picker (papp_ui_wifi)
                          sprite_demo = Sprite Demo: pad moves a 64x96 spaceship sprite over a space background, drawn by the PPA
                          (DISP v4 papp_disp_blit: RGB565 on magenta = key colour, or ARGB8888 with glow); modes Key on / Key off
                          (the whole magenta rectangle) / Alpha (radio buttons or A); the ship faces where it flies (quarter turns;
                          down = MIRROR_Y), L1/R1 = size 1/2x..2x in 1/16 steps; clear = papp_disp_fill; 3x zoom box; X exits.
                          cpu_blit = the same in software for older launchers. Dirty rectangles per buffer (edu_ui ui_begin_keep +
                          ui_pixels) for 30 fps on the 7"
                          lvgl_demo = LVGL Demo: the app links its own LVGL 9.2.2 (launcher/managed_components/lvgl__lvgl,
                          lv_conf.h beside main.c; edubuild.lvgl() adds the sources); LVGL draws straight into a 1-buffer
                          800x480 surface (direct mode), touch = pointer, pad + USB keyboard = keypad (one group per tab).
                          Tabs Widgets (slider/arc/switch/LED/button/dropdown colour/dark theme) / Chart / Text (lv_keyboard)
                          life = Game of Life (64x44 wrapping grid, touch draws, patterns); mandel_race = Mandelbrot timed
                          1 core / 2 cores / 2 cores + FAST / fixed point (workers at PAPP_PRIO_NORMAL: a HIGH one on the
                          main thread's core starves it before it starts the 2nd worker); synth = 2-octave multi-touch
                          keyboard, 5 waves, echo, scope + 512-point FFT (audio thread, 32 kHz); paint = multi-touch
                          canvas, flood fill, undo, BMP save/open in /sd/pictures (arg selftest); physics = balls as
                          ARGB8888 PPA blits (papp_disp_fill clears the box; CPU fallback), energy bar; dashboard =
                          LVGL tiles + chart: Wi-Fi RSSI/networks, BLE devices, free memory from a measuring thread,
                          Record = CSV in /sd/logs. Plan and ideas: docs/EDUCATION_PLAN.md
apps/<emu>/               Emulators, cores copied from RetroESP32_P4, each with a build.py:
                          nes (own glue), gb+gbc, sms+gg+col, pce, a78, spectrum, genesis, snes, lynx (C++),
                          a26 (C++), a800 (C++), neogeo. No args = ROM browser, or `-- <rom> [resume]`.
                          gba = gpSP (github.com/midworld/gpsp @3f112ae, RV32 JIT in core/riscv/, libretro core,
                          frontend gba_run.c on the odroid shim): JIT code in MEM v2 code memory (launcher >= 0.29.1),
                          else the interpreter; ROM paged in 32 KB pages (buffer = free PSRAM - 7 MB); BIOS optional
                          /sd/roms/gba/gba_bios.bin; saves .srm/.sav in /sd/odroid/data/gba; files nojit / romlimit / maxskip
                          there = debug switches. gpSP auto frameskip, fed by the audio ring level (odroid_audio_queued_ms < 30 ms),
                          at most 2 frames skipped in a row (gpsp_frameskip_max; gpSP had 30: Doom II, which emulates at ~45 fps,
                          showed 2 fps). The 5 s log line has fps, frames shown, ROM pages read and JIT flushes. Needs CONFIG_FATFS_USE_FASTSEEK (paging). In internal SRAM (PAPP_FAST_BSS, 128 KB): GBA iwram and the first 64 KB chunk of RAM JIT code (code chunks 64 KB; needs launcher >= 0.29.2, whose code_sync takes the FAST region; file nofastjit = off): Doom II 42 -> 47 fps; GBA_BENCH=1 or =JIT_DETAIL,INSTRET builds the profiler (5 s log lines). core/ patches: memalign'd
                          gamepak blocks, 32 KB aligned stdio buffer + seek return value in vfs_implementation.c
                          `python apps/build_all.py [name ...]` builds every app with a build.py
apps/common/odroid/       RetroESP32 odroid_* / app_common / FreeRTOS / ESP-IDF shims on the SDK, so the cores
                          and their *_run.c build unchanged; emu_main.c (browser → EMU_RUN; Exit game → PAPP_EXIT_RELAUNCH = back to the browser), emubuild.py;
                          in-game menu = odroid_menu.h (odroid_emu_menu / odroid_emu_volume, drawn by the launcher like
                          the HOME menu; NES has the same items in its own glue). X or the left screen edge = menu,
                          Y or the right edge = volume (the launcher's master volume); SNES, Genesis, Neo Geo set
                          odroid_input_xy_menu_disable (X/Y are game buttons): edges only
apps/common/icons/        88x88 icons of the emulator apps (emubuild copies <app>.png)
apps/common/v1compat/     v1 `app_services_t` host on the SDK + v1build.py: the RetroESP32 v1 PSRAM games
                          doom, opentyrian, quake, duke3d build with their own glue unchanged; frames go to the PPA without a copy (DISP v3 `present_ext`)
tools/papp_dev.py         DevLink host tool
tools/perf_bench.py       Benchmark (apps/perf_bench: CoreMark, PSRAM/SRAM bandwidth+latency, PPA, e3d 3D engine), screen on/off,
                          per board -> build/perf_bench/report.md; results v1.x vs v3.2: docs/BENCHMARK_V1_V3.md.
                          No build.py (not in build_all / the Store). PIE: v1.x = -march .._xesppie, v3.x = .._xespv (other
                          encodings: each faults on the other generation; SYS v4 papp_chip_rev() = major*100+minor tells
                          apps which, -1 before 0.33). e3d builds its SIMD set twice (e3d_simd_pie.c / _espv.c) and picks
                          one at e3d_create: one .papp for both. perf_bench = engine in C (-DE3D_NO_SIMD), perf_bench_simd
tools/build_c6_fw.py      C6 firmware matching a board's esp_hosted -> build/c6_fw/c6_<version>.bin (make_release adds it)
tools/make_release.py     Release files: launcher.bin per board (Settings → Update) + launcher_<id>_full.bin (first install at 0x0,
                          ESP Tool web page) + latest.json, prints the gh command
tools/make_apps_release.py  Catalogue for the Store from build/apps + community/ (+ apps/store.json: category and one-line
                          description of each app): apps.json + .papp + icons; --publish uploads
                          them to the "apps" pre-release (bump the app's version.txt first). --base + /sd/update/apps_url.txt
                          = test from a local server. --publish refuses unless community/, apps/store.json and gamepads.txt
                          match origin/main (committed) and no published app would vanish or go back a version (--force).
                          CI (.github/workflows/store.yml): a push to main changing community/, apps/store.json or
                          gamepads.txt runs --keep-apps --publish (project apps kept as published; their new versions still need a PC)
community/<dir>/          Community apps (PR with .papp + icon.png + app.json, no source): tools/community.py checks them
                          (.github/workflows/community.yml on PRs); catalogue "community" list, Community folder, not in Install all
SDcard/                   Starter card for new users (README.md): shareable game data, config/gamepads.txt, ROM folders.
                          Its .gitignore is an allow-list: ROMs, BIOS, saves and commercial data put there stay local
mechanical/               3D prints: 70° screw-on desk legs for the 7" board (M2.5 standoffs): .f3d, STEP, STL +
                          fusion_build_legs.py (Fusion script that builds them)
tests/host/test_reloc.py  Host relocation equivalence test
tests/board/smoke.py      Board regression: relocation test, test apps, crash record
```

## Environment

- ESP-IDF v5.5.6 at `C:\Users\97254\esp\v5.5.6\esp-idf` (5.5.2 still installed beside it); in PowerShell: `. .\tools\idf_env.ps1`
- Python with pyserial + pyelftools: `C:\Users\97254\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe`
- Boards (chip rev v1.x, `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, except the v3.2 7" `guition_p4_7_v3` on **COM6**); USB-Serial-JTAG belongs to DevLink, UART0 is the text console:
  Guition 4.3" `guition_p4_43` on **COM9** (build in `launcher/build`), Guition 7" JC1060P470C `guition_p4_7` on **COM10**
  (`idf.py -B build_guition_p4_7 -DLX_BOARD=guition_p4_7 -p COM10 build flash`). Porting: `docs/PORTING_GUIDE.md`.
  M5Stack Tab5 `m5stack_tab5` on **COM15** (`-B build_m5stack_tab5`; image "launcher_m5stack_tab5").
  Waveshare `waveshare_p4_rpi_dsi` (`-B build_waveshare_p4_rpi_dsi`; image "launcher_waveshare_p4_rpi_dsi"), tested on Linux:
  /dev/ttyACM0 = CH343 UART console and flashing, /dev/ttyACM1 = USB-Serial-JTAG (DevLink). Open the console without
  changing DTR/RTS (that resets the board); USB host works on the lower USB-A port only (no hub TT in IDF).
  `managed_components/` is shared and esp_hosted differs per board: after building another board, add `reconfigure`.
  launcher/CMakeLists.txt turns off the component manager's new-version check (newer esp_hosted releases broke configure).
  Every managed component is pinned to an exact version in the idf_component.yml files (dependencies.lock is not in git,
  it differs per board): a self-built launcher matches the release. Other ESP-IDF versions get a configure warning. 5.5.6 since launcher 0.25.3 (v3.x chips need >= 5.5.5).
  `papp_dev.py bootlog` shows what the board logged before DevLink came up (bring-up of a new board).

## Dev loop (works without touching the board)

```powershell
cd launcher; idf.py -p COM9 build flash; cd ..                     # launcher (only when it changes)
python papp-sdk\tools\papp_build.py --name hello --src apps\hello\main.c [--libc]
python tools\papp_dev.py --port COM9 deploy build\apps\hello\hello.papp --timeout 20 -- arg1
python tools\papp_dev.py --port COM9 crash --elf build\apps\hello\hello.elf    # after a panic reboot
python tests\host\test_reloc.py                                               # packer/relocator check
```

Apps in `/sd/apps/<dir>/<file>.papp` appear on the home screen; a directory there with no `.papp` is a folder of apps (its own `icon.png` optional)
(`/sd/apps/<folder>/<dir>/<file>.papp`, one level; `deploy` finds an app moved into one). Icon: an `icon.png` in the app's source
folder (PNG, square, best 88x88, transparency OK; corners are rounded by the launcher) is copied by
`papp_build.py` and uploaded by `deploy` to `/sd/apps/<dir>/icon.png`; without one the tile shows the initial.
`deploy` into a folder also uploads the folder's icon (`apps/common/icons/folder_<folder>.png`) when the folder has none. Every `papp_dev.py`
command also sets the board clock from the PC (`--no-time` skips it; `info` shows `time` and `rtc`).
`deploy` = upload to `/sd/apps/<name>/<name>.papp` → run → stream logs → exit code of the app
(124 timeout). `shot out.png` = screenshot of the visible framebuffer, in landscape (open it to see
the screen). Other commands: `info ls stat put get rm mkdir mv run stop status log reboot crash-clear`;
`net [--quick|--scan]` = network self-test on the board (link, pings, TCP, DNS, nearby networks).
`display off|on` = panel scan-out + backlight (bsp_display_scanout, Guition 7" only): no LCD refresh reads from PSRAM.
Input without touching the board: `input state [--watch 10]`, `input pad A+START [--hold ms]`,
`input key ENTER`, `input text "hi"`, or timed during a run: `deploy app.papp --inject 500:pad:A --inject 900:text:hi`.

## Rules

- Apps never include ESP-IDF headers; launcher core never includes board headers.
- New interface functions are **appended** and the interface version bumped; breaking change = new id.
- Every service entry point uses `SVC_ENTER/SVC_LEAVE` (forced stop waits for it) and tracks what it
  creates so teardown can reclaim it.
- SD transfers need 64-byte-aligned DMA buffers, or they run a sector at a time at 1.5 MB/s: launcher code that
  reads whole files uses `lx_file_fast_io` (free its buffer after `fclose`).
- `CONFIG_LX_APP_TEST_BASE_SHIFT_PAGES` must be 0 in normal builds (relocation test aid).
- Flash writes (NVS, OTA) must run on a task with an internal stack, never on an app thread: PSRAM stacks abort.
- After temporarily editing and restoring a source file, touch it: an old timestamp keeps the stale object.
- `ESP_SYSTEM_PMP_IDRAM_SPLIT` stays off (FAST region needs executable heap SRAM). After copying an
  sdkconfig file back, touch it: an old timestamp makes the build silently keep the previous config.
- Commit/push only when asked.

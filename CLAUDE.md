# ESP32-P4 Launcher — Project Context

Launcher firmware for ESP32-P4 boards that loads **relocatable PAPP v2 apps** from the SD card into
PSRAM and runs them with a DLL-style interface layer. Successor to the PAPP concept in
`C:\ESPIDFprojects\RetroESP32_P4` (reference project: drivers, ports, history).

Design: `docs/ARCHITECTURE.md`. Plan + status: `docs/WORKPLAN.md`.

## Layout

```
launcher/                 ESP-IDF project (factory app; main.c + home.c = LVGL home screen + app folders; store.c = the Store,
                          first home tile: categories, app pages, install/update/remove; home_ui.h = what it uses from home.c). Board: -DLX_BOARD=<id> (default guition_p4_43)
boards/<id>/board/        BSP implementation (component "board", pins esp_hosted for its C6); boards/<id>/sdkconfig.board.
                          guition_p4_43 (4.3" 480x800, image "launcher"; guition_p4_43_v3 = same sources built for
                          rev v3.x chips, BOARD_CHIP_V3 picks the id), guition_p4_7 (7" 1024x600), m5stack_tab5 (720x1280,
                          power rails on two PI4IOE5V6408 I2C expanders, esp_hosted 1.4 for M5Stack's C6 firmware)
components/bsp/           BSP contract (bsp.h) — the core only talks to hardware through it
components/papp_abi/      papp_abi.h — .papp v2 format + interfaces (shared with apps; no IDF includes)
components/lx_loader/     App Manager: loader, relocation, imports, services (SYS/MEM/FS/DISP/AUDI/INPT/THRD/UI/NET/BLE), teardown;
                          NET (lx_svc_net.c) = Wi-Fi, sockets, TLS, HTTP(S) for apps: the first call holds the radio
                          for the run (lx_net_app_hold), every wait gives up on exit, teardown closes sockets and rejoins the saved network;
                          BLE (lx_svc_ble.c) = NimBLE on the P4, controller on the C6 over esp_hosted (VHCI): the host
                          starts on the first call (gatt_register restarts it with the app's table) and stops at teardown;
                          Guition boards and Tab5 (CONFIG_BT_* in their sdkconfig.board; on Tab5's esp_hosted 1.4 the C6 runs its
                          controller from boot, lx_svc_ble.c stubs the 2.x controller calls), the slot is NULL on Waveshare
components/lx_devlink/    DevLink: framed protocol over USB-Serial-JTAG (devlink_proto.h)
components/lx_common/     crc32, launcher version
components/lx_gfx/        software drawing + 8x16 font for launcher screens (rotation-aware)
components/lx_comp/       compositor: app surfaces → PPA scale/rotate → panel; launcher screens draw via it
components/lx_audio/      audio engine: per-stream rings → resample → mix → BSP codec (fixed rate)
components/lx_ui/         launcher-drawn screens for apps: content browser (pick_file) + message box; UI v4 wifi() = Wi-Fi picker
                          (scan list, password keyboard, joins with lx_net_connect = saved to wifi.txt; lx_net_restore on fail/cancel)
components/lx_input/      input manager: USB HID gamepads/keyboards, XInput (Xbox 360) pads (lx_input_xinput) + board buttons (merged into P1) → players + event queue; HOME chord;
                          lx_hid_parse = HID descriptor parser; lx_padmap = per-pad button mapping as text on the card:
                          /sd/config/gamepads_mine.txt (Map controller) wins over /sd/config/gamepads.txt (shared database,
                          SDcard/config/gamepads.txt, downloaded by the Store); NVS "padmap" = pre-0.23, read-only fallback
components/lx_update/     self-update from /sd/update/launcher.bin + rollback confirm; online update (latest.json);
                          lx_appstore = app catalogue for the Store (apps.json on the "apps" release; icons cached in /sd/launcher/store)
components/lx_net/        Wi-Fi station, NTP, time zone via the ESP32-C6 (esp_hosted 2.12.13 on the Guition boards, 1.4 on Tab5/Waveshare);
                          lx_c6 = the C6's firmware: version (DevLink info c6_fw) and update from /sd/update/c6.bin or the release;
                          saved network = /sd/config/wifi.txt (ssid=/password= lines; NVS "net" = pre-0.24, moved to the card at boot)
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
                          periph = "gution-p4" for ESP32P4_Gution4_7project/tools/ble_client.py — all test apps are
                          PAPP_MF_HIDDEN: shown on the home screen only with Settings → Show developer apps)
apps/education/<app>/     Education apps for students (ids edu.*): `python apps/education/build.py` (build_all runs it too)
                          builds them into build/education and copies .papp + icon to SDcard/apps/Education/<app>/ (committed;
                          the "Education" home screen folder, icon from apps/common/icons/folder_education.png). Never in the Store
                          catalogue (make_apps_release reads build/apps only and skips edu.*). common/edu_ui.c (linked into each):
                          touch UI on an 800x480 surface, log panel, the remotes' shared state + Control screen.
                          ble_remote = BLE Remote, the Gution BLE demo ("gution-p4": control service mode/level/led/msg + Nordic
                          UART console, same protocol as ESP32P4_Gution4_7project's GutionTerminal); wifi_remote = Wi-Fi Remote,
                          web server on port 80 (page, GET/POST /state in the Gution JSON, GET /set?level=..), hotspot
                          "P4-Remote-xxxx" (password p4remote) when there is no network or on the Hotspot button; Network button /
                          "Choose network" = the launcher's Wi-Fi picker (papp_ui_wifi)
apps/<emu>/               Emulators, cores copied from RetroESP32_P4, each with a build.py:
                          nes (own glue), gb+gbc, sms+gg+col, pce, a78, spectrum, genesis, snes, lynx (C++),
                          a26 (C++), a800 (C++), neogeo. No args = ROM browser, or `-- <rom> [resume]`.
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
tools/build_c6_fw.py      C6 firmware matching a board's esp_hosted -> build/c6_fw/c6_<version>.bin (make_release adds it)
tools/make_release.py     Release files: launcher.bin per board (Settings → Update) + launcher_<id>_full.bin (first install at 0x0,
                          ESP Tool web page) + latest.json, prints the gh command
tools/make_apps_release.py  Catalogue for the Store from build/apps + community/ (+ apps/store.json: category and one-line
                          description of each app): apps.json + .papp + icons; --publish uploads
                          them to the "apps" pre-release (bump the app's version.txt first). --base + /sd/update/apps_url.txt
                          = test from a local server. --publish refuses unless community/, apps/store.json and gamepads.txt
                          match origin/main (committed) and no published app would vanish or go back a version (--force)
community/<dir>/          Community apps (PR with .papp + icon.png + app.json, no source): tools/community.py checks them
                          (.github/workflows/community.yml on PRs); catalogue "community" list, Community folder, not in Install all
SDcard/                   Starter card for new users (README.md): shareable game data, config/gamepads.txt, ROM folders.
                          Its .gitignore is an allow-list: ROMs, BIOS, saves and commercial data put there stay local
tests/host/test_reloc.py  Host relocation equivalence test
tests/board/smoke.py      Board regression: relocation test, test apps, crash record
```

## Environment

- ESP-IDF v5.5.6 at `C:\Users\97254\esp\v5.5.6\esp-idf` (5.5.2 still installed beside it); in PowerShell: `. .\tools\idf_env.ps1`
- Python with pyserial + pyelftools: `C:\Users\97254\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe`
- Boards (both chip rev v1.x, `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`); USB-Serial-JTAG belongs to DevLink, UART0 is the text console:
  Guition 4.3" `guition_p4_43` on **COM9** (build in `launcher/build`), Guition 7" JC1060P470C `guition_p4_7` on **COM10**
  (`idf.py -B build_guition_p4_7 -DLX_BOARD=guition_p4_7 -p COM10 build flash`). Porting: `docs/PORTING_GUIDE.md`.
  M5Stack Tab5 `m5stack_tab5` on **COM15** (`-B build_m5stack_tab5`; image "launcher_m5stack_tab5").
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
`papp_build.py` and uploaded by `deploy` to `/sd/apps/<dir>/icon.png`; without one the tile shows the initial. Every `papp_dev.py`
command also sets the board clock from the PC (`--no-time` skips it; `info` shows `time` and `rtc`).
`deploy` = upload to `/sd/apps/<name>/<name>.papp` → run → stream logs → exit code of the app
(124 timeout). `shot out.png` = screenshot of the visible framebuffer, in landscape (open it to see
the screen). Other commands: `info ls stat put get rm mkdir mv run stop status log reboot crash-clear`;
`net [--quick|--scan]` = network self-test on the board (link, pings, TCP, DNS, nearby networks).
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

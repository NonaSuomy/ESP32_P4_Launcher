# Porting the launcher to a new board

The launcher core never touches hardware directly: it goes through the BSP contract in
`components/bsp/include/bsp.h`. A board is one folder, `boards/<id>/`, and apps (`.papp`) never
change: they ask for the display size, draw into their own surface, and the compositor scales and
rotates it onto the panel. The same SD card runs on every board.

Written from the second and third ports, the Guition JC1060P470C (7" 1024x600, `boards/guition_p4_7`)
and the M5Stack Tab5 (720x1280, `boards/m5stack_tab5`), each done in an afternoon.

## 1. The board package

```
boards/<id>/
├── sdkconfig.board            PSRAM, flash, chip revision, esp_hosted wiring for this board
└── board/
    ├── CMakeLists.txt         component "board", WHOLE_ARCHIVE, REQUIRES bsp + the drivers it uses
    ├── idf_component.yml      managed components this board needs, and the esp_hosted version
    ├── board_pins.h           every pin and size in one place
    ├── board.c                bsp_info (id, name, caps, panel size, display_rot), I2C bus, SD card
    ├── board_display.c        panel bring-up, 2 RGB565 framebuffers, flip on vsync, backlight
    ├── board_touch.c          points in landscape coordinates (apply display_rot here)
    ├── board_audio.c          codec at one fixed rate, 16-bit stereo
    ├── board_buttons.c        board buttons, paddle, battery; return "none" if the board has none
    └── board_rtc.c            real-time clock in UTC; ESP_ERR_NOT_SUPPORTED if the board has none
```

Start by copying the closest existing board. What each function must do is written next to it in
`bsp.h`.

## 2. Checklist

1. **Board facts.** Chip revision (`esptool chip_id`: v1.x needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`),
   panel controller, resolution and native orientation, touch controller, codec and amplifier pin,
   SD card wiring, USB host port, ESP32-C6 wiring. A vendor demo or another project that runs on the
   board is the best source; copy its init tables rather than guessing.
2. **`bsp_info`.** `id` is used by updates and must be unique. `display_w/h` is the panel in its native
   orientation; `display_rot` is how the landscape picture maps onto it (0 for a landscape panel, 270
   for the 4.3" portrait panel).
3. **Display.** Two framebuffers, `bsp_display_show()` switches scan-out at the next refresh,
   `bsp_display_wait_flip()` waits for it. Clear both buffers before the backlight comes on.
4. **Touch.** Report landscape coordinates scaled to the panel. The GT911 reports its own configured
   range (800x480 on the 7" board), so scale from that, not from the panel size. On the 7" board an
   I2C probe right after the panel reset upsets the GT911: talk to its addresses directly.
5. **Audio.** One fixed rate (48 kHz). The launcher's mixer resamples every app stream to it.
6. **Wi-Fi companion.** `esp_hosted` must suit the firmware on the board's ESP32-C6, so its version is
   pinned in the board's `idf_component.yml`: 2.0 on the 4.3", 2.12 on the 7" (its C6 reports 2.3), and
   1.4 with `esp_wifi_remote` 0.8.5 on the Tab5, as in M5Stack's demo (2.12 never got past SDIO card
   init there). `lx_net` only asks for `esp_wifi_remote` without a version. SDIO pins other than the
   P4 slot 1 defaults are set in `sdkconfig.board`; check the build log line `sdio_wrapper: GPIOs: ...`,
   since a setting with the wrong name for that esp_hosted version is silently ignored (on 1.4, D1 is
   `ESP_HOSTED_SDIO_PRIV_PIN_D1_4BIT_BUS`).
7. **Power rails.** Some boards switch the display reset, speaker, USB 5 V and C6 power through I2C
   I/O expanders (the Tab5 has two PI4IOE5V6408). Set them up first thing in `bsp_init`, copying the
   vendor's order and values.
8. **Real-time clock.** `bsp_rtc_get/set` in UTC. The launcher sets the system time from it at boot,
   and saves NTP time to it (and DevLink time when a time zone is set, which makes that real UTC).
9. **Capabilities.** Set `BSP_CAP_*` as each device comes up, so the launcher hides what is missing
   (no battery: the system bar and Settings leave it out).

## 3. Build and flash

Each board has its own build folder and sdkconfig. The component lock file (`dependencies.lock`, not in
git) and `managed_components/` are shared: when you switch to another board, run `reconfigure` once
(`idf.py -B build_<id> -DLX_BOARD=<id> reconfigure build`), or ninja looks for the other board's esp_hosted files:

```powershell
cd launcher
idf.py -B build_<id> -DLX_BOARD=<id> -p COMxx build flash
```

The image's project name is `launcher_<id>` (the first board, `guition_p4_43`, keeps `launcher` and
builds in `build/`). An update refuses an image whose project name differs, so a board never installs
another board's launcher, from the SD card or online.

## 4. Test

1. `python tools/papp_dev.py --port COMxx info` shows the board id, display size and capabilities.
   `bootlog` shows everything logged before DevLink came up: the place to look when a device doesn't start.
2. `shot` screenshots the home screen: check the layout uses the whole display.
3. Put the SD card from another board in, unchanged. Run apps of each kind: an emulator (NES), a v1
   game (Doom, Quake), an LVGL app (Clock). HOME must open the in-game menu.
4. `python tests/board/smoke.py --port COMxx` (relocation test, test apps, crash record).
5. Settings → Wi-Fi: scan, join, `papp_dev.py net` for the network self-test.
6. Copy another board's `launcher.bin` to `/sd/update/`: the board must refuse it at boot.

## 5. Releases

`python tools/make_release.py` packs every board built at the current version into one release:
`latest.json` lists each under `"boards"`, and its top-level fields are `guition_p4_43`'s, where
launchers from before per-board releases look. A board with no entry is told there is no release for
it yet.

## Known differences between the two boards

| | Guition 4.3" | Guition 7" (JC1060P470C) |
|---|---|---|
| Panel | ST7701 480x800 portrait, `display_rot` 270 | JD9165 1024x600 landscape, `display_rot` 0 |
| Panel reset | GPIO 5 | GPIO 27 (V1 panel), GPIO 0 (2026 V2 panel: needs the other init table) |
| Backlight | on/off | PWM |
| Audio | ES8311, amplifier on GPIO 11 | ES8311, NS4150 amplifier on GPIO 20 |
| C6 firmware | esp_hosted 2.0.17 | esp_hosted 2.3.0 (host 2.12 works, with a version warning) |
| Extras | battery pin, GPIO pad add-on | RX8025T real-time clock (keeps UTC; set from NTP) |

The M5Stack Tab5: 720x1280 portrait panel (`display_rot` 90), one of three, told apart at boot by the
touch controller: ILI9881C with GT911 touch, or ST7123 / ST7121 with touch built in at 0x55 (its
firmware version register: 1 = ST7121, 3 = ST7123; init tables from M5Stack's BSP), ES8388
codec, RX8130 RTC, two PI4IOE5V6408 expanders (0x43: speaker amp, LCD reset; 0x44: C6 power, USB 5 V,
charging), C6 on SDIO CLK 12, CMD 13, D0-D3 11/10/9/8, reset 15. Its 2-cell battery (INA226 at 0x41)
isn't reported yet.

The Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 (`boards/waveshare_p4_35`, pins from Waveshare's BSP
component 2.0.2): 320x480 ST7796 on SPI (80 MHz), FT6336 touch at 0x38, ES8311 (amplifier on GPIO 53),
SDMMC with on-chip LDO 4, C6 on the default SDIO pins (CLK 18, CMD 19, D0-D3 14-17, reset 54), esp_hosted
1.4. The panel has its own GRAM, so the board emulates the double-buffered contract: the framebuffers are
a 768x512 virtual screen (the home screen needs about 800x480), and a push task scales each frame by
0.625 with the PPA and streams it to the panel through two DMA bounce buffers (byte-swapped, the panel
is big-endian). About 30 fps at most. Touch is mapped back up to 768x512. These boards ship with
ESP32-P4 **v3.2** silicon, which needs ESP-IDF 5.5.5 or later (Espressif advisory AR2026-004): on
5.5.2 it boot-loops on LP_WDT resets before the bootloader prints a line.

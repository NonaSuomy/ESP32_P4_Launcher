# Port: Waveshare ESP32-P4 Module Dev Kit + Raspberry Pi DSI touch display (800x480)

Board id `waveshare_p4_rpi_dsi` (`boards/waveshare_p4_rpi_dsi/`, image `launcher_waveshare_p4_rpi_dsi`;
`waveshare_p4_rpi5` before 2026-11-07, renamed before any release because "rpi5" reads as Raspberry Pi 5).

The panel is one of the many displays built like the official Raspberry Pi Touch Display (V1):
TC358762 DSI-to-DPI bridge, ATTINY88 at 0x45 for power and backlight, FT5x06 touch, 800x480. Tested
with a Cuqi 5" "Touchscreen for Raspberry Pi". The official 7" display has the same design and
timings (Linux `rpi_touchscreen` driver: about 26 MHz pixel clock), so it will likely work too, untested.

The board facts come from the esp32-scummvm port, which runs on this hardware:
<https://github.com/chneeb/esp32-scummvm/blob/esp32p4-ev/CLAUDE.md> and its forked BSP
(`backends/platform/esp32/bsp_override/esp32_p4_function_ev_board_noglib/`).

## Status

| Phase | State |
|---|---|
| 0. Facts checked on the board | done: chip v1.3; DevLink on the "USB" Type-C, console (CH343) on the other; C6 on the EV-board SDIO pins, firmware reports 0.0.0 (version warning with host 2.12, but scan and join work) |
| 1. Board package | runs: touch (FT5x06 fw 0x0B), audio, SD, USB host, Wi-Fi, DevLink |
| 2. Display (TC358762, RGB888 scan-out) | runs, survives software reboots; games reach the panel's 46.7 Hz (blend conversion, below) |
| 3. Build integration and docs | done: README (contributed boards), CLAUDE.md, PORTING_GUIDE, WORKPLAN |
| 4. Tests | partly, see Phase 4 |

## Board facts

| | Waveshare kit + RPi V1-style panel | Same as the Guition 7"? |
|---|---|---|
| Panel | TC358762 DSI-to-DPI bridge, 1 lane at 600 Mbps, non-burst with sync pulses, 26 MHz, about 46.7 Hz, 800x480 landscape (`display_rot` 0) | no |
| Panel power, resets, backlight | ATTINY88 at I2C 0x45 (PORTA/B/C, PWM register 0x86) | no |
| Touch | FT5x06 at 0x38, polled, mirrored in X and Y, reset through the ATTINY | no (GT911) |
| I2C | SDA 7, SCL 8 | yes |
| Audio | ES8311, I2S MCLK 13 / BCLK 12 / WS 10 / DOUT 9 / DIN 11, amplifier GPIO 53 | yes, except the amp pin (20) |
| SD | slot 0, 4-bit, 39-44, LDO channel 4 | yes |
| DSI PHY | LDO channel 3, 2500 mV | yes |
| Flash / PSRAM | 16 MB QIO, 32 MB PSRAM at 200 MHz | mostly (DIO) |
| USB host | HS root port only (the lower J8 USB-A, next to the Ethernet jack); the hub ports fail (no Transaction Translator in IDF) | - |
| Wi-Fi / BLE | ESP32-C6 on SDIO, esp_hosted 2.12; BLE for apps after the C6 update to the release's 2.12.13 firmware (Settings > Update) | yes |
| RTC, battery, buttons | none (BOOT button possible) | - |

## Phase 0: check on the board

1. `esptool chip_id`: the chip revision. `sdkconfig.board` assumes v1.x
   (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`); a v3 chip needs that changed.
2. DevLink: USB-Serial-JTAG should be the Type-C labelled "USB" (GPIO 24/25), with the UART
   console on the other Type-C. Check that `papp_dev.py info` answers there.
3. ESP32-C6: Function EV board SDIO wiring (CLK 18, CMD 19, D0-D3 14-17, reset 54) is assumed
   (`CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`). Read the slave firmware version from the boot log and
   pin esp_hosted to match: 2.0.17 as on the 4.3", or 2.12 as on the 7" (the current choice).
4. The private `esp_lcd_dsi_bus_t` layout (`bus_id`, `hal`) was checked against IDF 5.5.2's
   and 5.5.6's `esp_lcd/dsi/mipi_dsi_priv.h` (the same in both); recheck it on any IDF upgrade.
   5.5.6 renames the DPI `on_refresh_done` callback to `on_frame_buf_complete` (the old name is kept
   as an alias in a union) and still calls it from the DMA trans-done interrupt, as before.

## Phase 1: board package

Copied from `guition_p4_7` (same Function EV board wiring):

- `sdkconfig.board`: QIO 16 MB, PSRAM 200 MHz, chip revision, `ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`.
- `idf_component.yml`: `esp_codec_dev` and esp_hosted / esp_wifi_remote, pinned exact.
- `board_pins.h`, `board.c` (id, name, I2C bus, SD), `board_audio.c` (amp on GPIO 53).
- `board_touch.c`: FT5x06 driver; points mirrored in both axes.
- `board_rtc.c`, `board_buttons.c`: none.

## Phase 2: display

Bring-up order, as in the esp32-scummvm BSP (don't reorder):

1. ATTINY: PWM 0, PORTC 0 (all resets held), PORTA LCD_LR, PORTB LCD_MAIN, PORTC LED_EN.
2. DSI PHY LDO, DSI bus (1 lane, 600 Mbps).
3. A DBI IO created and deleted (sets the LP clock).
4. Raw DPI panel in RGB888, 2 framebuffers; then non-burst with sync pulses and no frame ACK
   (`esp_lcd_new_panel_dpi` sets burst), then `esp_lcd_panel_init`.
5. Continuous HS clock, no command ACK.
6. ATTINY releases the bridge and LCD, SYSPMCTRL wake through the ATTINY's register proxy.
7. TC358762 registers by DSI generic long writes (`LCDCTRL = 0x00100150`: RGB888).
8. ATTINY releases the touch reset; backlight through the PWM register. The backlight stays dark
   below PWM 39-40 (on the tested Cuqi 5"), so `bsp_display_backlight` maps 1-100 % onto 44-255
   (0 = off): before, everything up to 15 % was dark, the home screen's idle dim (15 %) included.

**RGB565 contract vs RGB888 panel.** RGB565 to the bridge wraps the picture about 200 px sideways
(a TC358762 FIFO phase issue). The contract stays RGB565: `bsp_display_fb()` returns two RGB565
shadow buffers, and `bsp_display_show(i)` has the PPA convert shadow `i` into RGB888 scan-out buffer
`i` (hidden, since callers wait for the last flip first), then switches scan-out to it. The PPA's
565 -> 888 expansion gives the panel's byte order without a swap. Cost: 1.5 MB PSRAM and one PPA pass
per frame. `lx_comp`, `lx_gfx`, the UI takeover and screenshots work unchanged.

If the extra pass shows up in Phase 4: add `bsp_display_format()` to the contract and let `lx_comp`
write RGB888 straight from its PPA pass, keeping the shadow only for launcher screens.

**46.7 Hz.** Anything that paces on `bsp_display_wait_vsync` runs at about 47 fps. Big timing changes
blank this panel, so a faster pixel clock needs care.

**Measured (Super Star Wars, PAL).** These numbers came from a timing log in `bsp_display_show()`
(frames, fps, and cache / 565->888 / switch / max ms per frame, every 5 s while frames are shown).
It was removed after the measurements; to bring it back, take `s_st`, `STATS_US` and the
`esp_timer_get_time()` stamps in `bsp_display_show()` from commit `6f76046`
(`git show 6f76046:boards/waveshare_p4_rpi5/board/board_display.c`, the board's folder before
the rename). The PPA SRM
(scaler) conversion of 800x480 took about 18 ms per frame; with the compositor's 4 ms scale pass a
frame needed 22 ms, just over one refresh (21.4 ms), so the screen got 23-24 fps while the emulator
ran at 50. The PPA blend engine does the same conversion in about 8.4 ms (background = the RGB565
frame, foreground = an all-transparent A4 picture): 12.5 ms per frame, 44-48 fps on screen, every
refresh the panel has. At boot a test pattern goes through both engines and the bytes are compared;
blend is used only if they match (the SRM output order is the one esp32-scummvm confirmed on this
panel), otherwise SRM. Cache write-back and buffer switch cost 0.04 ms. If more headroom is ever
needed: the compositor writing RGB888 directly (one pass, contract change).

**Recovery.** A bad bring-up can wedge the bridge's PLL until the display is power-cycled (it is
powered from its own connector). Software reboots (updates, crash records) must bring it back: test
it; if not, hold the ATTINY resets longer in `attiny_power_on`.

## Phase 3: build integration and docs (done)

```
cd launcher
idf.py -B build_waveshare_p4_rpi_dsi -DLX_BOARD=waveshare_p4_rpi_dsi reconfigure build
idf.py -B build_waveshare_p4_rpi_dsi -DLX_BOARD=waveshare_p4_rpi_dsi -p <port> flash
```

The board is in `CLAUDE.md` (layout, environment), `README.md` (contributed boards, build line, the USB
port to use), `docs/PORTING_GUIDE.md` (RGB888 panels, Linux ports, board notes) and `docs/WORKPLAN.md`.
`tools/make_release.py` picks up every folder under `boards/` by itself.

**Linux host.** Tested from Linux: `/dev/ttyACM0` is the CH343 UART console (and `idf.py flash`),
`/dev/ttyACM1` the USB-Serial-JTAG with DevLink. Linux raises DTR and RTS when a port opens, and
`papp_dev.py` used to drop DTR first, which resets the chip on every command; it now drops RTS first
on Linux and macOS (Windows unchanged). A serial monitor on the console port must not change DTR/RTS
either.

## Phase 4: tests (PORTING_GUIDE §4)

With ESP-IDF 5.5.6 and launcher 0.26.0, 2026-11-07. Done: 1 (`info`, `bootlog`, `shot`, colours
checked by eye; the blend conversion self-check passes, 8.0 ms a frame); 2 in part (NES, SNES,
Genesis, Game Boy, Doom, Clock); 3 in part (touch, USB pad on the lower port); 4 (`smoke.py`: host
relocation, hello, libc_test, hid_test, thread_test, no crash record; Wi-Fi join; `papp_dev.py net`:
gateway, pings, TCP, DNS; the Guition 7"'s `launcher_guition_p4_7.bin` in `/sd/update/` refused,
"not a launcher image for this chip and board"); 5 in part (software and panic reboots bring the
panel back); 6 (2026-11-09: 16 % = PWM 40 still lit, 15 % = PWM 38 dark, hence the mapping above).
Open: Quake (no game data on the test card), the HOME menu, sound and volume, power cycle.

1. `info`, `bootlog`; `shot` of the home screen: full 800x480, colours right (red/blue not swapped
   on the panel itself: the screenshot reads the RGB565 shadow, so check by eye too).
2. SD card from another board as is: NES, Doom/Quake (`present_ext`), Clock (LVGL); HOME menu.
3. Touch in all four corners; audio and volume; a USB pad on the lower J8 port.
4. `tests/board/smoke.py`; Wi-Fi scan/join and `papp_dev.py net`; another board's `launcher.bin`
   in `/sd/update/` must be refused.
5. Software reboot, panic reboot and power cycle: the panel comes back each time.
6. Backlight dimming (Settings brightness, idle dim).

## Optional extras

- M5Stack CardKB on the Qwiic port (0x5F) as a keyboard in `lx_input`: needs a BSP hook appended
  to the contract (the core never includes board headers).
- Ethernet (IP101 PHY) as a second network path in `lx_net`.

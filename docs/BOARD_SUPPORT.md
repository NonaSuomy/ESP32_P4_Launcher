# Board support

The launcher's promise to a user is: buy this board, flash one file, copy the SD card, and everything works:
screen, touch, sound, Wi-Fi, the Store and every app. A board is supported only when we can keep that
promise for it, release after release. This page says which boards qualify and how to add one.

## What a board needs

A board must be **one product you can buy**, with a name and a SKU, not a combination of parts. Everything
below must be on the board itself:

| | Requirement | Why |
|---|---|---|
| SoC | ESP32-P4 | the launcher and every app are built for it |
| Memory | 32 MB PSRAM, 16 MB flash | apps load into PSRAM (GBA, Quake, Duke3D need most of it); two launcher slots for updates |
| Display | built-in MIPI-DSI panel, 320×480 or larger, with touch | the browser, the menus and touch apps need it |
| Wi-Fi | an ESP32-C6 (or another esp_hosted co-processor) wired to the P4 | the Store, Settings → Update, network time, Wi-Fi and BLE apps |
| Storage | a microSD slot | apps, ROMs and saves live on the card |
| Audio | a speaker output (codec or I2S amplifier) | emulators and games |
| Input | a USB host port | gamepads and keyboards |

Not supported:

- **Development kits where the user picks the panel.** A kit with a DSI connector can take many displays, each
  with its own controller, timing and touch chip, and one image can't know which is connected. Such a port can
  live in your fork, but it is not built into releases.
- **Boards without Wi-Fi.** Without it there is no Store, no updates and no network apps, and the board would
  need its own instructions for everything. Supporting it would mean a second way to install and update.

## Two levels

| Level | Meaning | Boards today |
|---|---|---|
| **Supported** | Built into every release and tested on the board before each one | Guition ESP32-P4 4.3" (`guition_p4_43`, and `guition_p4_43_v3` for v3.x chips), Guition JC1060P470C 7" (`guition_p4_7`, and `guition_p4_7_v3` for v3.x chips), M5Stack Tab5 (`m5stack_tab5`) |
| **Community** | Built into every release; tested by the person who owns the board, who confirms each release or says it broke | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 (`waveshare_p4_35`), Elecrow All-in-one Starter Kit for ESP32-P4 (`elecrow_p4_aio`, not tested yet) |

A community board that nobody tests for three releases in a row is marked "not tested" in the release notes,
and is removed if it breaks and no one with the board can fix it.

### Retired

- **Waveshare ESP32-P4 dev kit + Raspberry Pi DSI display** (`waveshare_p4_rpi_dsi`): a development kit where
  the user chooses the panel. Launcher 0.30.0 is its last release image; after that, Settings → Update on it says
  there is no release for the board (`tools/make_release.py` lists it in `RETIRED`). The board package stays in `boards/` as an example port (an RGB888-only panel behind a bridge chip,
  converted by the PPA: [PORT_WAVESHARE_P4.md](PORT_WAVESHARE_P4.md)), and you can still build it yourself.

## Adding a board

1. **Open an issue first** ("New board" template) with the product name and SKU, a link to its schematic, its
   PSRAM and flash size, its Wi-Fi chip and how the chip is wired (SDIO or SPI, which pins). We check it meets
   the list above before you spend time on a port.
2. **Port it**, following [PORTING_GUIDE.md](PORTING_GUIDE.md): one folder, `boards/<id>/`, copied from the closest
   existing board. The launcher core and the apps don't change. Use pinned versions in `idf_component.yml`.
3. **Test it on the board** and attach the results to the pull request:
   - `python tests/board/smoke.py --port COMx` passes (relocation test, test apps, crash record)
   - `python tools/papp_dev.py --port COMx shot home.png` of the home screen, and a screenshot of one emulator
   - Wi-Fi joins and the Store installs an app; Settings → Update finds the current release
   - sound plays, touch hits where you tap, a USB gamepad works
4. **Document it**: a `docs/PORT_<BOARD>.md` with what differs from the other boards, a line in the README's
   board tables, and the board in `CLAUDE.md`'s layout list.
5. **Stay with it**: when a release is coming, flash it on your board and say in the release issue whether it
   works. That is what keeps a community board in the releases.

## Release check

Before a release, every **supported** board runs `tests/board/smoke.py` and starts at least one emulator from
the Store; every **community** board's owner is asked to do the same. `tools/make_release.py` builds an image for
each board under `boards/` except the retired ones it lists.

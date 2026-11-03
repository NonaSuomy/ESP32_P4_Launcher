# Elecrow AIO and Waveshare Nano board ports

These ports adapt the board wiring and panel sequences used by the PAX workspace.
They are separate BSPs for this launcher. The launcher accepts the same PAPP2 app
container under either a `.papp` or `.pax` filename. Treat these as experimental
hardware ports; only the Elecrow AIO has been flashed so far.

| Setting | `elecrow_p4_aio` | `waveshare_nano_lcd43` |
|---|---|---|
| Display | AIO 7-inch 1024×600, 180° mounting | Waveshare Raspberry Pi DSI 4.3-inch, 800×480 |
| MIPI | 2 lanes, 1000 Mbps, 51 MHz pixel clock | 1 lane, 800 Mbps, 26 MHz; RGB565 framebuffer, RGB888 wire |
| Touch | GT911, I2C18/19, reset40, interrupt/address strap41; raw touch axes are independent of the display's 180° rotation | FT5x06 0x38, I2C7/8, both axes mirrored |
| Backlight | PWM20, 30 kHz | I2C panel controller 0x45, register0x86 |
| SD | slot1, 1-bit CLK43/CMD44/D039 | slot0, same 1-bit pins, LDO4 and active-low power45 |
| Audio | external I2S WS21/BCLK22/DOUT23, active-low PA6 | ES8311 0x18, MCLK13/BCLK12/WS10/DOUT9, active-high PA53 |
| C6 | **full-duplex SPI** MOSI7/MISO9/CLK15/CS14, ready10/handshake11/reset2 | SDIO slot1 CLK18/CMD19/D014/D115/D216/D317/reset54 |
| Ethernet | IP101 address1, MDC31/MDIO52/reset51, external RMII clock50 | Same |
| Extra inputs | calibrated ADC1 channel0 (GPIO16) ladder | USB/touch |

Both default to 16 MB DIO flash at 80 MHz and the pre-v3 P4 rev1.x target,
matching the fitted PAX units. Check a different unit's chip revision/flash before
using those defaults. Nano power must be sufficient for its attached screen.
The Nano BSP does not cover the 10.1-inch or HDMI adapters.

The AIO C6 must have firmware for the SPI full-duplex wiring above; an SDIO or
half-duplex image will not work. The host pins esp_hosted 2.12.13 and
esp_wifi_remote 1.6.5, as the existing 7-inch port does. A PAX Bluetooth companion
is not installed or replaced by building this branch; Bluetooth services are not
implemented by this launcher.

## Build

Use ESP-IDF v5.5.2:

```sh
python tools/enable_exfat.py  # after sourcing ESP-IDF's export.sh; one-time per IDF checkout
python tools/apply_p4_usb_tt_fix.py "$IDF_PATH"  # after sourcing export.sh; one-time per IDF checkout
cd launcher
idf.py -B build_elecrow_p4_aio -DLX_BOARD=elecrow_p4_aio build
idf.py -B build_waveshare_nano_lcd43 -DLX_BOARD=waveshare_nano_lcd43 build
cd ..
python tests/host/test_pax_boards.py
```

ESP-IDF 5.5 hard-codes FatFs exFAT support off and exposes no Kconfig switch.
The helper enables `FF_FS_EXFAT` in the selected `IDF_PATH` checkout; it is
idempotent and refuses to patch an unfamiliar FatFs configuration. Keep long
file names enabled. On an ESP-IDF release that provides `CONFIG_FATFS_FS_EXFAT`,
enable that option instead of using the helper.

If switching an already configured AIO build from different radio defaults,
regenerate its ignored `sdkconfig.elecrow_p4_aio`: existing sdkconfig values take
precedence over defaults. The test reads the generated config and catches that
case, including esp_hosted's derived `SPI_GPIO_*` pins.

The launcher vendors PAX's USB host with hub transaction-translator split
support. On ESP32-P4, the HAL helper above configures the HS UTMI root port for
FS/LS-only operation and applies matching 30 MHz frame timing. It patches the
selected ESP-IDF 5.5 checkout; rerun it after replacing that checkout. This
allows the AIO hub to pass full-speed devices instead of rejecting them as
unsupported TT children.

Each BSP has its own image/project name, so the existing updater's board check
continues to prevent cross-board installs. `tools/make_release.py` discovers the
new board directories automatically when their images have been built.

## Implementation notes and hardware validation

`boards/pax_p4_common` holds code shared only by these two BSPs. Existing boards
retain weak no-Ethernet defaults. Network initialization starts wired DHCP, gives
Ethernet priority with netif route priority 150 (Wi-Fi STA is 100), and keeps
Wi-Fi state separate so losing the Ethernet address exposes the Wi-Fi state
again. Forget Wi-Fi affects Wi-Fi credentials only. The wired interface remains
available while the launcher pauses its Wi-Fi radio for a game.

Both audio paths hold the amplifier disabled during codec/clock setup and prime
six silent DMA blocks before allowing the first PCM write to enable it. Muting
disables the amplifier; the AIO scales PCM because it has no controllable codec.

The display waits conservatively for two refresh callbacks before reusing the
old front buffer. Measure frame pacing before reducing this to one. Test panel
orientation and all four touch corners, SD remount/reboot, USB controllers
directly and behind the AIO hub, mute and cold audio startup, C6 scan/connect,
Ethernet DHCP and cable removal/fallback.
The Elecrow AIO was flashed and booted on P4 revision v1.3 at 80 MHz. The display
initialized at 1024×600, the GT911 answered at 0x5D and reported touch coordinates,
and the launcher reached Home without a reboot loop. Live use showed touch actions
were 180° out; the touch driver now leaves the GT911 axes unrotated while the display
compositor retains its 180° image rotation. The SD card failed to mount during this
test, so app discovery could not yet be checked on the device. The Nano remains
untested on hardware. After flashing the PAX TT-capable host and P4 HAL workaround,
the user confirmed the Switch controller works through the AIO hub; the serial
log showed the hub and controller enumerated at full speed.

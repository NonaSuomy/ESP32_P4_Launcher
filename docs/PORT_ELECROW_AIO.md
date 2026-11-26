# Elecrow All-in-one Starter Kit for ESP32-P4 (SEE00804D)

Board id `elecrow_p4_aio`, image `launcher_elecrow_p4_aio`. **Written without the hardware: not tested yet.**

```powershell
cd launcher; idf.py -B build_elecrow_p4_aio -DLX_BOARD=elecrow_p4_aio -p COMx build flash
```

## Sources

- Elecrow's ESP-IDF lessons, [Elecrow-RD/All-in-one-Starter-Kit-for-ESP32-P4-with-Common-Board-design](https://github.com/Elecrow-RD/All-in-one-Starter-Kit-for-ESP32-P4-with-Common-Board-design)
  (`example/V1.0/idf-code`: Kconfig defaults and sdkconfig of each lesson), and the sdkconfig of its AI project
  (`example/AI Projects/ESP-IDF_Code`) for flash, PSRAM, chip revision and the ESP32-C6 wiring.
- NonaSuomy's port in pull request #2 (button ladder thresholds, measured on the kit).

## Hardware

| Part | Setting |
|---|---|
| Display | 7" 1024×600 EK79007, MIPI-DSI 2 lanes at 900 Mbps, 51 MHz pixel clock, HPW/HBP/HFP 70/160/160, VPW/VBP/VFP 10/23/12; reset GPIO 5; backlight PWM GPIO 20 (30 kHz); DSI PHY on LDO3 2.5 V |
| Orientation | scan direction pins UPDN GPIO 32 = 0, SHLR GPIO 33 = 1 (Elecrow's levels) → `display_rot` 0. `BOARD_LCD_FLIP 1` in `board_pins.h` inverts both pins and turns touch to match |
| Touch | GT911 on I2C0, SDA 18, SCL 19, reset 40, INT 41 (reset with INT low → 0x5D; 0x14 tried too) |
| Audio | I2S0 BCLK 22, WS 21, DOUT 23 straight into an amplifier, no codec: volume is scaled in software. Amplifier enable GPIO 6, on when low (off from `bsp_init` until silent DMA runs) |
| SD | SDMMC slot 0, 1-bit, CLK 43, CMD 44, D0 39, card supply LDO4 3.3 V; 20 MHz default. On timeout/CRC, CMD12 recovers a card stuck in DATA/RCV and retries once. |
| Buttons | resistor ladder on GPIO 16 (ADC1 channel 0): left, right, down ("middle" in Elecrow's code), up; diagonals below 1.6 V |
| Wi-Fi / BLE | ESP32-C6 over SPI full duplex: MOSI 7, MISO 9, CLK 15, CS 14, data ready 10, handshake 11, reset GPIO 2 active high |
| Flash / PSRAM | 16 MB QIO 80 MHz, 32 MB hex PSRAM at 200 MHz, chip rev v1.x (`ESP32P4_SELECTS_REV_LESS_V3`) |
| RTC, battery | none |
| DevLink | CH340 USB-UART bridge on UART0 at 115200 baud; `papp_dev.py` auto-detects it (for example `/dev/ttyUSB0`); use `python tools/papp_dev.py --port /dev/ttyUSB0 info` |

The C6 runs esp_hosted over SPI full duplex, as on PR #2's kit (launcher 0.30.0 tried SDIO, the transport of
Elecrow's own AI demo, and found no C6). The release's C6 firmware is built for SDIO, so Settings → Update offers
none for this board: keep the C6's SPI firmware, or build one for SPI full duplex with these pins and flash it over
the C6's own port.
PR #2's Ethernet (IP101) is not part of this port.

## To check on the kit

1. Boot log (`papp_dev.py bootlog`, or UART0): display, GT911 address, SD mount, `I2S amplifier ready`.
2. Picture upright and touch on the finger; if both are upside down set `BOARD_LCD_FLIP 1`. If the picture is
   right but touch is not, the GT911's axes need their own fix in `board_touch.c`.
3. SD card: speed in the boot log (40 MHz or the 20 MHz retry); copy a large file with `papp_dev.py put`.
4. Sound: a game or the volume setting, no pop at boot or exit.
5. Buttons: `papp_dev.py input state --watch 10` while pressing each one; the thresholds are in `board_buttons.c`.
6. Wi-Fi: Settings → Wi-Fi, `papp_dev.py net`. Then BLE (`ble_test`); BLE needs a C6 firmware that carries
   BLE over VHCI.
7. Chip revision: `esptool chip_id`. A v3.x chip needs a v3 build (`ESP32P4_SELECTS_REV_LESS_V3` off).

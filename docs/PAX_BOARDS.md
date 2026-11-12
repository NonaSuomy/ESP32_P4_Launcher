# Elecrow ESP32-P4 AIO board port

This adds a board support package for the Elecrow ESP32-P4 AIO used by the PAX
project. It is a launcher firmware port for this hardware; it does not add legacy
PAPP1 loading.

| Setting | Elecrow AIO |
|---|---|
| Display | 7-inch, 1024×600 DSI, mounted at 180° |
| MIPI | 2 lanes, 1000 Mbps, 51 MHz pixel clock |
| Touch | GT911, I2C18/19, reset GPIO40; raw axes are independent of panel rotation |
| Backlight | PWM GPIO20, 30 kHz |
| SD | slot 1, 1-bit CLK43/CMD44/D039 |
| Audio | I2S BCLK22/WS21/DOUT23, active-low amplifier GPIO6 |
| C6 | SPI full duplex: MOSI7/MISO9/CLK15/CS14, ready10/handshake11/reset2 |
| Ethernet | IP101, PHY address 1; MDC31/MDIO52/reset51, external RMII clock50 |
| Extra input | ADC1 channel 0 (GPIO16) button ladder |

The supplied sdkconfig targets the fitted 16 MB flash, 80 MHz DIO, P4 revision
1.0 or newer and 32 MB hex PSRAM. Check another unit's flash and chip revision
before using those defaults. The C6 needs firmware built for the SPI full-duplex
wiring above; an SDIO or half-duplex image will not work.

## Build

Use ESP-IDF v5.5.2:

```sh
cd launcher
idf.py -B build_elecrow_p4_aio -DLX_BOARD=elecrow_p4_aio build
cd ..
python tests/host/test_pax_boards.py
```

For an existing build directory, regenerate its ignored
`sdkconfig.elecrow_p4_aio` if it was configured for a different radio interface;
existing sdkconfig values take precedence over defaults. The host test checks the
resolved C6 SPI pins and image identity after the build.

The new image has been booted on the AIO. The display and rotated picker were
checked, and the GT911 touch orientation fix was made from live interaction. Full
SD, audio, Ethernet failover and cold-boot regression checks should be completed
before treating the port as fully validated.

`boards/pax_p4_common` contains the shared PAX board drivers. Existing boards keep
weak no-Ethernet defaults. Ethernet starts with DHCP and uses route priority 150;
Wi-Fi remains available as fallback. The amplifier stays muted while clocks and
silent DMA buffers are established at boot.

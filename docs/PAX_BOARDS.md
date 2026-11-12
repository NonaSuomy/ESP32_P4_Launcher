# Elecrow AIO and Waveshare Nano board ports

These board support packages add Elecrow ESP32-P4 AIO and Waveshare Nano support
for this launcher. They do not add legacy PAPP1 loading.

| Setting | Elecrow AIO | Waveshare Nano LCD43 |
|---|---|---|
| Display | 7-inch, 1024×600 DSI, mounted at 180° | 4.3-inch DSI, 800×480, RGB888 scanout |
| MIPI | 2 lanes, 1000 Mbps, 51 MHz pixel clock | 1 lane, 800 Mbps, 26 MHz pixel clock |
| Touch | GT911, I2C18/19, reset GPIO40; raw axes are independent of panel rotation | FT5x06 at 0x38, I2C7/8, mirrored axes |
| Backlight | PWM GPIO20, 30 kHz | I2C panel controller 0x45, register 0x86 |
| SD | slot 1, 1-bit CLK43/CMD44/D039 | slot 0, 1-bit CLK18/CMD19/D014, LDO4 and active-low power GPIO45 |
| Audio | I2S BCLK22/WS21/DOUT23, active-low amplifier GPIO6 | ES8311 at 0x18, MCLK13/BCLK12/WS10/DOUT9, active-high amplifier GPIO53 |
| C6 | SPI full duplex: MOSI7/MISO9/CLK15/CS14, ready10/handshake11/reset2 | SDIO: CLK18/CMD19/D014/D115/D216/D317/reset54 |
| Ethernet | IP101, PHY address 1; MDC31/MDIO52/reset51, external RMII clock50 | Same |
| Extra input | ADC1 channel 0 (GPIO16) button ladder | USB and touch |

The supplied sdkconfig targets the fitted 16 MB flash, 80 MHz DIO, P4 revision
1.0 or newer and 32 MB hex PSRAM. Check another unit's flash and chip revision
before using those defaults. The C6 needs firmware built for the SPI full-duplex
wiring above; an SDIO or half-duplex image will not work.

## Build

Use ESP-IDF v5.5.2:

```sh
cd launcher
idf.py -B build_elecrow_p4_aio -DLX_BOARD=elecrow_p4_aio build
idf.py -B build_waveshare_nano_lcd43 -DLX_BOARD=waveshare_nano_lcd43 build
cd ..
python tests/host/test_pax_boards.py
```

For an existing build directory, regenerate its ignored
`sdkconfig.elecrow_p4_aio` if it was configured for a different radio interface;
existing sdkconfig values take precedence over defaults. The host test checks the
resolved C6 SPI pins and image identity after the build.

The AIO image booted on the physical board; its display, picker orientation and
GT911 touch orientation were checked through live use. The Nano image was also
flashed; the panel became readable after matching its working DSI timing and RGB888
scanout. Remaining checks include SD remount, audio startup, C6 networking, Ethernet
failover and longer cold-boot runs on both boards.

`boards/pax_p4_common` contains the shared PAX board drivers. Existing boards keep
weak no-Ethernet defaults. Ethernet starts with DHCP and uses route priority 150;
Wi-Fi remains available as fallback. The amplifier stays muted while clocks and
silent DMA buffers are established at boot.

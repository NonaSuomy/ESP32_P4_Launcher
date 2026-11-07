/* Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 (320x480 ST7796 SPI, FT6336, ES8311) pin map.
 * Taken from Waveshare's BSP component waveshare/esp32_p4_wifi6_touch_lcd_3_5 v2.0.2. */
#pragma once

/* Panel scanned in landscape (MADCTL MV set): 480x320. The launcher's layout needs at least
 * ~800x480, so the board presents a 768x512 virtual screen (same 3:2 shape) and the PPA
 * scales each frame by exactly 0.625 onto the panel. */
#define BOARD_LCD_H_RES     480
#define BOARD_LCD_V_RES     320
#define BOARD_VIRT_W        768
#define BOARD_VIRT_H        512
#define BOARD_LCD_SPI_HOST  SPI2_HOST
#define BOARD_LCD_MOSI      20
#define BOARD_LCD_CLK       21
#define BOARD_LCD_CS        23
#define BOARD_LCD_DC        26
#define BOARD_LCD_RST       27
#define BOARD_LCD_BL_GPIO   28
#define BOARD_LCD_PCLK_HZ   (80 * 1000 * 1000)
/* Landscape orientation: MV alone or MV+MX+MY (180 degrees apart). Flip this if the
 * picture comes out upside down. */
#define BOARD_LCD_FLIP_180  0

#define BOARD_I2C_PORT      1       /* shared by FT6336, ES8311 and the AXP2101 */
#define BOARD_I2C_SDA       7
#define BOARD_I2C_SCL       8

#define BOARD_TP_ADDR       0x38
#define BOARD_TP_RST        29
#define BOARD_TP_INT        50
#define BOARD_TP_RAW_W      320     /* FT6336 reports portrait panel pixels */
#define BOARD_TP_RAW_H      480

#define BOARD_I2S_MCLK      13
#define BOARD_I2S_BCLK      12
#define BOARD_I2S_WS        10
#define BOARD_I2S_DOUT      9
#define BOARD_I2S_DIN       11
#define BOARD_AUDIO_PA      53
#define BOARD_AUDIO_RATE    48000

#define BOARD_SD_CLK        43
#define BOARD_SD_CMD        44
#define BOARD_SD_D0         39
#define BOARD_SD_D1         40
#define BOARD_SD_D2         41
#define BOARD_SD_D3         42
#define BOARD_SD_LDO_CHAN   4

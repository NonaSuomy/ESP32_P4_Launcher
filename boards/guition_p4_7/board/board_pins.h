/* Guition JC1060P470C (ESP32-P4 7" 1024x600 JD9165 MIPI-DSI, GT911, ES8311 + NS4150) pin map.
 * Taken from the ESP32_P4_Internet_Radio project, which runs on this board. */
#pragma once

#define BOARD_LCD_H_RES     1024
#define BOARD_LCD_V_RES     600
#define BOARD_LCD_BL_GPIO   23      /* PWM */
/* The 2026 "New Panel" (rear label "V2"; every board with a rev v3.x chip has it, some v1.x ones too)
 * needs its own JD9165 table and timing and resets on GPIO 0; the V1 panel resets on GPIO 27.
 * board_display.c tells them apart at start-up. */
#define BOARD_LCD_RST_GPIO_V1   27
#define BOARD_LCD_RST_GPIO_NEW  0
#define BOARD_DSI_PHY_LDO_CHAN 3
#define BOARD_DSI_PHY_LDO_MV   2500

#define BOARD_TP_I2C_SDA    7
#define BOARD_TP_I2C_SCL    8

#define BOARD_I2S_MCLK      13
#define BOARD_I2S_BCLK      12
#define BOARD_I2S_WS        10
#define BOARD_I2S_DOUT      9
#define BOARD_I2S_DIN       11
#define BOARD_AUDIO_PA      20      /* NS4150 enable */

#define BOARD_SD_CLK        43
#define BOARD_SD_CMD        44
#define BOARD_SD_D0         39
#define BOARD_SD_D1         40
#define BOARD_SD_D2         41
#define BOARD_SD_D3         42
#define BOARD_SD_LDO_CHAN   4

#define BOARD_I2C_PORT      1       /* shared by GT911 and ES8311 */
#define BOARD_AUDIO_RATE    48000

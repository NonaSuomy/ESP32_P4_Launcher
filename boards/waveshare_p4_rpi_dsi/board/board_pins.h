/* Waveshare ESP32-P4-Module-DEV-KIT (Function EV board wiring) with an RPi 7" V1-style 5" 800x480
 * DSI panel: TC358762 DSI-to-DPI bridge, ATTINY88 power/backlight MCU, FT5x06 touch; ES8311 codec.
 * Taken from the esp32-scummvm port (esp32p4-ev branch), which runs on this board. */
#pragma once

#define BOARD_LCD_H_RES     800
#define BOARD_LCD_V_RES     480
#define BOARD_DSI_PHY_LDO_CHAN 3
#define BOARD_DSI_PHY_LDO_MV   2500
#define BOARD_DSI_LANES     1
#define BOARD_DSI_LANE_MBPS 600

#define BOARD_ATTINY_ADDR   0x45    /* panel power, bridge/touch reset, backlight PWM */
#define BOARD_TOUCH_ADDR    0x38    /* FT5x06 */

#define BOARD_I2C_SDA       7
#define BOARD_I2C_SCL       8

#define BOARD_I2S_MCLK      13
#define BOARD_I2S_BCLK      12
#define BOARD_I2S_WS        10
#define BOARD_I2S_DOUT      9
#define BOARD_I2S_DIN       11
#define BOARD_AUDIO_PA      53      /* power amplifier enable */

#define BOARD_SD_CLK        43
#define BOARD_SD_CMD        44
#define BOARD_SD_D0         39
#define BOARD_SD_D1         40
#define BOARD_SD_D2         41
#define BOARD_SD_D3         42
#define BOARD_SD_LDO_CHAN   4

#define BOARD_I2C_PORT      1       /* shared by the ATTINY88, FT5x06 and ES8311 */
#define BOARD_AUDIO_RATE    48000

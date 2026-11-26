/* Elecrow All-in-one Starter Kit for ESP32-P4 (SKU SEE00804D): 7" 1024x600 EK79007 MIPI-DSI, GT911,
 * I2S amplifier without codec, 1-bit SD, ADC button ladder, ESP32-C6 over SPI.
 * Pins from Elecrow's ESP-IDF lessons (github.com/Elecrow-RD/All-in-one-Starter-Kit-for-ESP32-P4-with-
 * Common-Board-design, example/V1.0/idf-code: Kconfig defaults and sdkconfig) and NonaSuomy's port (#2).
 * NOT TESTED on the hardware yet. */
#pragma once

#define BOARD_LCD_H_RES     1024
#define BOARD_LCD_V_RES     600
#define BOARD_LCD_BL_GPIO   20      /* PWM, 30 kHz in Elecrow's code */
#define BOARD_LCD_RST_GPIO  5
#define BOARD_LCD_UPDN_GPIO 32      /* panel scan direction pins: Elecrow drives UPDN 0, SHLR 1 */
#define BOARD_LCD_SHLR_GPIO 33
/* 1 = picture upside down with Elecrow's levels: both pins inverted, touch turned to match */
#define BOARD_LCD_FLIP      0
#define BOARD_DSI_PHY_LDO_CHAN 3
#define BOARD_DSI_PHY_LDO_MV   2500
#define BOARD_DSI_LANE_MBPS 900
#define BOARD_PCLK_MHZ      51
#define BOARD_HPW           70
#define BOARD_HBP           160
#define BOARD_HFP           160
#define BOARD_VPW           10
#define BOARD_VBP           23
#define BOARD_VFP           12

#define BOARD_I2C_PORT      0       /* GT911 (and the kit's sensor modules) */
#define BOARD_TP_I2C_SDA    18
#define BOARD_TP_I2C_SCL    19
#define BOARD_TP_RST        40
#define BOARD_TP_INT        41

#define BOARD_I2S_BCLK      22
#define BOARD_I2S_WS        21
#define BOARD_I2S_DOUT      23
#define BOARD_AUDIO_PA      6       /* amplifier on when LOW */
#define BOARD_AUDIO_RATE    48000

#define BOARD_SD_CLK        43      /* slot 0 (IOMUX pins), 1-bit */
#define BOARD_SD_CMD        44
#define BOARD_SD_D0         39
#define BOARD_SD_LDO_CHAN   4       /* 3.3 V, the SD pins' supply (Elecrow sets LDO4 to 3300 mV) */

#define BOARD_KEY_ADC_CHAN  0       /* ADC1 channel 0 = GPIO16: resistor ladder */

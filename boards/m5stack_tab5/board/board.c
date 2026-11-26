/*
 * BSP implementation — M5Stack Tab5 (ESP32-P4, 5" 720x1280).
 * Shared I2C bus, the two I/O expanders that switch the board's power rails,
 * SD card and board info here; display, touch, audio and RTC in their own files.
 */
#include "board_internal.h"
#include "esp_bit_defs.h"
#include "board_pins.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

static bsp_info_t s_info = {
    .id        = "m5stack_tab5",
    .name      = "M5Stack Tab5 1280x720",
    .caps      = BSP_CAP_USB_HOST |     /* USB-A host port (5 V switched by IOX2 P3); the rest is set at init */
                 BSP_CAP_KEYBOARD,      /* the clip-on Tab5 Keyboard, looked for while the launcher runs */
    .display_w = BOARD_LCD_H_RES,
    .display_h = BOARD_LCD_V_RES,
    .display_rot = 90,                  /* portrait panel, used in landscape */
};

static bool s_sd_mounted;
static i2c_master_bus_handle_t s_i2c;

/* PI4IOE5V6408 registers */
#define IOX_RESET     0x01
#define IOX_DIR       0x03      /* 1 = output */
#define IOX_OUT       0x05
#define IOX_OUT_HIZ   0x07      /* 1 = high impedance */
#define IOX_IN_DEF    0x09
#define IOX_PULL_EN   0x0B
#define IOX_PULL_UP   0x0D      /* 1 = pull-up, 0 = pull-down */
#define IOX_INT_MASK  0x11

static i2c_master_dev_handle_t iox_add(uint16_t addr)
{
    i2c_master_dev_handle_t d = NULL;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(s_i2c, &cfg, &d) != ESP_OK) return NULL;
    return d;
}

static void iox_set(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    i2c_master_transmit(d, b, 2, 50);
}

/* Same order and values as M5Stack's bsp_io_expander_pi4ioe_init (M5Tab5-UserDemo) */
static void expanders_init(void)
{
    i2c_master_dev_handle_t x1 = iox_add(BOARD_IOX1_ADDR), x2 = iox_add(BOARD_IOX2_ADDR);
    if (!x1 || !x2) {
        ESP_LOGE(TAG, "I/O expanders missing: no display, sound, Wi-Fi or USB power");
        return;
    }
    /* IOX1: P1 speaker amp on; P4 is the LCD reset, which takes no 3.3 V push-pull: drive it
     * low for 10 ms, then release it as an input with pull-up */
    iox_set(x1, IOX_RESET, 0xFF);
    iox_set(x1, IOX_PULL_UP, 0b01111111);
    iox_set(x1, IOX_PULL_EN, 0b01111111);
    iox_set(x1, IOX_OUT, 0b01100110);
    iox_set(x1, IOX_OUT_HIZ, 0b00000000);
    iox_set(x1, IOX_DIR, 0b01111111);
    vTaskDelay(pdMS_TO_TICKS(10));
    iox_set(x1, IOX_DIR, 0b01101111);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* IOX2: P0 C6 Wi-Fi power and P3 USB 5 V out on, charging off; P6 an input */
    iox_set(x2, IOX_RESET, 0xFF);
    iox_set(x2, IOX_DIR, 0b10111001);
    iox_set(x2, IOX_OUT_HIZ, 0b00000110);
    iox_set(x2, IOX_PULL_UP, 0b10111001);
    iox_set(x2, IOX_PULL_EN, 0b11111001);
    iox_set(x2, IOX_IN_DEF, 0b01000000);
    iox_set(x2, IOX_INT_MASK, 0b10111111);
    iox_set(x2, IOX_OUT, 0b00001001);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t r1 = IOX_OUT, o1 = 0, r2 = IOX_OUT, o2 = 0, d2 = 0, rd = IOX_DIR;
    i2c_master_transmit_receive(x1, &r1, 1, &o1, 1, 50);
    i2c_master_transmit_receive(x2, &r2, 1, &o2, 1, 50);
    i2c_master_transmit_receive(x2, &rd, 1, &d2, 1, 50);
    ESP_LOGI(TAG, "I/O expanders set: IOX1 out %02x, IOX2 out %02x dir %02x (Wi-Fi power = IOX2 P0)", o1, o2, d2);
}

esp_err_t bsp_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c);
    if (err != ESP_OK) ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
    else {
        expanders_init();
        bsp_power_init();
    }
    return ESP_OK;      /* devices report their own absence */
}

/* GT911 reset: TP_RST is I/O expander 1 P5, the touch INT is GPIO 23. INT high while the
 * reset rises selects address 0x14 (M5Stack's choice); then INT low for 50 ms, the GT911's
 * synchronisation, and released as an input. The GT911 then loads its configuration. */
void board_touch_reset(void)
{
    i2c_master_dev_handle_t x1 = iox_add(BOARD_IOX1_ADDR);
    if (!x1) return;
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_TOUCH_INT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(BOARD_TOUCH_INT, 1);
    uint8_t reg = IOX_OUT, out = 0;
    i2c_master_transmit_receive(x1, &reg, 1, &out, 1, 50);
    iox_set(x1, IOX_OUT, out & ~(1 << 5));
    vTaskDelay(pdMS_TO_TICKS(10));
    iox_set(x1, IOX_OUT, out | (1 << 5));
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(BOARD_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(50));
    i2c_master_bus_rm_device(x1);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

void board_set_cap(uint32_t cap)
{
    s_info.caps |= cap;
}

const bsp_info_t *bsp_info(void)
{
    return &s_info;
}

esp_err_t bsp_sdcard_mount(const char *mount_point)
{
    if (s_sd_mounted) return ESP_OK;

    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = BOARD_SD_LDO_CHAN };
    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD LDO init failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int attempt = 1; attempt <= 3; attempt++) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
        host.pwr_ctrl_handle = pwr;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = BOARD_SD_CLK;
        slot.cmd = BOARD_SD_CMD;
        slot.d0  = BOARD_SD_D0;
        slot.d1  = BOARD_SD_D1;
        slot.d2  = BOARD_SD_D2;
        slot.d3  = BOARD_SD_D3;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 16,
            .allocation_unit_size = 16 * 1024,
        };

        sdmmc_card_t *card = NULL;
        err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mount_cfg, &card);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SD mounted at %s (%s, %llu MB)", mount_point, card->cid.name,
                     ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20);
            s_sd_mounted = true;
            board_set_cap(BSP_CAP_SDCARD);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SD mount attempt %d failed: %s", attempt, esp_err_to_name(err));

        /* Power-cycle the card through the LDO before retrying */
        sd_pwr_ctrl_del_on_chip_ldo(pwr);
        pwr = NULL;
        if (attempt == 3) break;
        vTaskDelay(pdMS_TO_TICKS(500));
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (pwr) sd_pwr_ctrl_del_on_chip_ldo(pwr);     /* free for a later try */
    return err;
}

bool bsp_sdcard_mounted(void)
{
    return s_sd_mounted;
}

/* Pins for apps (IRQ interface), from M5Stack's schematic: the M5-Bus on the back (SPI 5/18/19, Port B
 * 16/17/45/52, Port C 6/7, and 2/3/4/47/48/51), G49 on Ext.Port1 and Grove Port A (53/54). Ext.Port1's
 * G0/G1/G50 are the clip-on keyboard's (board_keyboard.c); 31/32 (system I2C), 35 (BOOT), 37/38 (console)
 * and the RS485 pins 20/21/34 are not apps'. */
uint64_t bsp_app_gpio_mask(void)
{
    return BIT64(2) | BIT64(3) | BIT64(4) | BIT64(5) | BIT64(6) | BIT64(7) | BIT64(16) | BIT64(17) | BIT64(18) | BIT64(19) | BIT64(45) | BIT64(47) | BIT64(48) | BIT64(49) | BIT64(51) | BIT64(52) | BIT64(53) | BIT64(54);
}

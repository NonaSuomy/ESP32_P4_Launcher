/*
 * BSP implementation — Guition ESP32-P4 4.3" (reference board).
 * SD card, shared I2C bus and board info here; display, touch and audio in
 * their own files. Capability bits are set as each device comes up.
 */
#include "board_internal.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

/* boards/guition_p4_43_v3 builds these sources for a rev v3.x chip, under its own id */
#ifdef BOARD_CHIP_V3
#define BOARD_ID   "guition_p4_43_v3"
#define BOARD_NAME "Guition ESP32-P4 4.3\" 480x800 (chip v3)"
#else
#define BOARD_ID   "guition_p4_43"
#define BOARD_NAME "Guition ESP32-P4 4.3\" 480x800"
#endif

static bsp_info_t s_info = {
    .id        = BOARD_ID,
    .name      = BOARD_NAME,
    .caps      = BSP_CAP_USB_HOST,      /* USB-A port on the OTG HS PHY; the rest is set at init */
    .display_w = BOARD_LCD_H_RES,
    .display_h = BOARD_LCD_V_RES,
    .display_rot = 270,
};

static bool s_sd_mounted;
static i2c_master_bus_handle_t s_i2c;

esp_err_t bsp_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_TP_I2C_SDA,
        .scl_io_num = BOARD_TP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c);
    if (err != ESP_OK) ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
    return ESP_OK;      /* touch / audio report their own absence */
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

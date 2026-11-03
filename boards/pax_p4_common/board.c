// SPDX-License-Identifier: MIT
#include "board_internal.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
static const char *TAG = "pax_bsp";
static bsp_info_t info = {.id = BOARD_ID,
                          .name = BOARD_NAME,
                          .caps = BSP_CAP_USB_HOST,
                          .display_w = BOARD_LCD_H_RES,
                          .display_h = BOARD_LCD_V_RES,
                          .display_rot = BOARD_LCD_ROT};
static i2c_master_bus_handle_t bus;
static bool mounted;
static esp_ldo_channel_handle_t rails[2];
i2c_master_bus_handle_t board_i2c_bus(void) { return bus; }
void board_set_cap(uint32_t cap) { info.caps |= cap; }
const bsp_info_t *bsp_info(void) { return &info; }
esp_err_t bsp_init(void) {
  // Amplifier stays muted until clocks and silent DMA have been established.
  gpio_set_level(BOARD_AUDIO_PA, !BOARD_PA_ACTIVE);
  gpio_set_direction(BOARD_AUDIO_PA, GPIO_MODE_OUTPUT);
  esp_ldo_channel_config_t rail = {.chan_id = 3, .voltage_mv = 2500};
  ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&rail, &rails[0]), TAG,
                      "DSI LDO3");
#if BOARD_AIO
  rail.chan_id = 4;
  ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&rail, &rails[1]), TAG,
                      "AIO LDO4");
#endif
  i2c_master_bus_config_t cfg = {.i2c_port = BOARD_I2C_PORT,
                                 .sda_io_num = BOARD_TP_I2C_SDA,
                                 .scl_io_num = BOARD_TP_I2C_SCL,
                                 .clk_source = I2C_CLK_SRC_DEFAULT,
                                 .glitch_ignore_cnt = 7,
                                 .flags.enable_internal_pullup = BOARD_AIO};
  return i2c_new_master_bus(&cfg, &bus);
}
esp_err_t bsp_sdcard_mount(const char *path) {
  if (mounted)
    return ESP_OK;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = BOARD_SD_SLOT;
  /* Match the PAX AIO's conservative clock. The 64-GB exFAT card is less
   * reliable at the 20-MHz default on this wiring. */
  host.max_freq_khz = 10000;
#if !BOARD_AIO
  static sd_pwr_ctrl_handle_t supply;
  if (!supply) {
    sd_pwr_ctrl_ldo_config_t cfg = {.ldo_chan_id = 4};
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&cfg, &supply), TAG,
                        "SD LDO4");
  }
  host.pwr_ctrl_handle = supply;
  gpio_set_level(BOARD_SD_POWER, 0);
  gpio_set_direction(BOARD_SD_POWER, GPIO_MODE_OUTPUT);
  vTaskDelay(pdMS_TO_TICKS(20));
#endif
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = BOARD_SD_CLK;
  slot.cmd = BOARD_SD_CMD;
  slot.d0 = BOARD_SD_D0;
  slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_sdmmc_mount_config_t cfg = {.format_if_mount_failed = false,
                                          .max_files = 16,
                                          .allocation_unit_size = 16384};
  sdmmc_card_t *card = NULL;
  esp_err_t err = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; attempt++) {
    err = esp_vfs_fat_sdmmc_mount(path, &host, &slot, &cfg, &card);
    if (err == ESP_OK) {
      mounted = true;
      board_set_cap(BSP_CAP_SDCARD);
      ESP_LOGI(TAG, "SD mounted at %s: %s, %llu MB, %d kHz", path,
               card->cid.name,
               (unsigned long long)card->csd.capacity * card->csd.sector_size /
                   (1024 * 1024),
               card->real_freq_khz);
      break;
    }
    ESP_LOGW(TAG, "SD mount attempt %d/3 failed: %s (0x%x)", attempt,
             esp_err_to_name(err), (unsigned)err);
    if (attempt < 3)
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  return err;
}
bool bsp_sdcard_mounted(void) { return mounted; }

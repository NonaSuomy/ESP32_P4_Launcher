/*
 * ES8311 codec + I2S0, configured once at BOARD_AUDIO_RATE (16-bit stereo) and
 * never reconfigured. Output only for now; the PA is switched by esp_codec_dev.
 */
#include "board_internal.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "audio";

#define MCLK_MULTIPLE 256

static i2s_chan_handle_t s_tx;
static esp_codec_dev_handle_t s_codec;

esp_err_t bsp_audio_init(void)
{
    if (s_codec) return ESP_OK;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    if (i2c_master_probe(bus, ES8311_CODEC_DEFAULT_ADDR >> 1, 50) != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 not found on I2C");
        return ESP_ERR_NOT_FOUND;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;         /* underrun plays silence, not the last buffer */
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 480;       /* 10 ms per descriptor at 48 kHz */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    esp_err_t err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }

    audio_codec_i2c_cfg_t i2c_cfg = { .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = s_tx };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_AUDIO_PA,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        .mclk_div = MCLK_MULTIPLE,
    };
    const audio_codec_if_t *codec_if = ctrl_if && data_if && gpio_if ? es8311_codec_new(&es_cfg) : NULL;
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = codec_if ? esp_codec_dev_new(&dev_cfg) : NULL;
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = BOARD_AUDIO_RATE,
    };
    if (!s_codec || esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 setup failed");
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_codec, 60);

    board_set_cap(BSP_CAP_AUDIO);
    ESP_LOGI(TAG, "ES8311 ready, %d Hz 16-bit stereo", BOARD_AUDIO_RATE);
    return ESP_OK;
}

int bsp_audio_rate(void)
{
    return BOARD_AUDIO_RATE;
}

size_t bsp_audio_write(const int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    if (!s_codec) return 0;
    size_t written = 0;
    i2s_channel_write(s_tx, stereo, frames * 4, &written, pdMS_TO_TICKS(timeout_ms));
    return written / 4;
}

void bsp_audio_volume(int percent)
{
    if (!s_codec) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    esp_codec_dev_set_out_vol(s_codec, percent);
}

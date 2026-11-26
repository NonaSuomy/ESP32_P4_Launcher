/*
 * I2S0 straight into the speaker amplifier (no codec): 16-bit stereo at BOARD_AUDIO_RATE, configured once.
 * The amplifier's enable (BOARD_AUDIO_PA) is active low; it is switched on only after the I2S clocks
 * run with silent DMA, so the boot makes no pop. Volume is applied to the samples here.
 */
#include "board_internal.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define CHUNK_FRAMES 256

static i2s_chan_handle_t s_tx;
static volatile int s_gain = 154;       /* 0..256 (60 %, like the codec boards' start volume) */

esp_err_t bsp_audio_init(void)
{
    if (s_tx) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;         /* underrun plays silence, not the last buffer */
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 480;       /* 10 ms per descriptor at 48 kHz */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(30));      /* a few silent DMA buffers out first */
    gpio_set_level(BOARD_AUDIO_PA, 0);  /* amplifier on */

    board_set_cap(BSP_CAP_AUDIO);
    ESP_LOGI(TAG, "I2S amplifier ready, %d Hz 16-bit stereo", BOARD_AUDIO_RATE);
    return ESP_OK;
}

int bsp_audio_rate(void)
{
    return BOARD_AUDIO_RATE;
}

size_t bsp_audio_write(const int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    if (!s_tx) return 0;
    static int16_t buf[CHUNK_FRAMES * 2];
    const int gain = s_gain;
    size_t done = 0;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    while (done < frames) {
        size_t n = frames - done;
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
        const int16_t *src = stereo + done * 2;
        for (size_t i = 0; i < n * 2; i++) buf[i] = (int16_t)((src[i] * gain) >> 8);
        size_t written = 0;
        i2s_channel_write(s_tx, buf, n * 4, &written, ticks);
        done += written / 4;
        if (written < n * 4) break;     /* timed out */
    }
    return done;
}

void bsp_audio_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_gain = percent * 256 / 100;
}

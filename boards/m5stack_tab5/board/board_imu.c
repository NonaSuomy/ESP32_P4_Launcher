/* The Tab5's accelerometer (BMI270 at 0x68 on the internal I2C bus), for apps that are tilted: offered through
 * bsp_analog_read() as three inputs, BSP_ANALOG_ACCEL_X/Y/Z, each 0..65535 with 32768 at 0 g and 4096 to the g
 * (the sensor's own numbers at its +-8 g range, moved up by 32768). The axes are the chip's own, which is what
 * M5Unified gives on this board.
 *
 * Nothing touches the sensor until an app first asks: it shares its bus with the touch controller, and most
 * apps never tilt. The first read resets it and loads its configuration file, which takes about a quarter of
 * a second, in pieces so that touch reads get the bus in between. After that a read is one short transaction,
 * and readings are reused for 10 ms. */
#include <string.h>
#include "board_internal.h"
#include "bmi270_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_imu";

#define BMI_ADDR          0x68
#define REG_CHIP_ID       0x00      /* 0x24 */
#define REG_STATUS        0x03      /* bit 7: a new accelerometer sample */
#define REG_ACC_X_LSB     0x0C
#define REG_INTERNAL_STAT 0x21      /* low nibble 1: configuration loaded and running */
#define REG_ACC_CONF      0x40
#define REG_ACC_RANGE     0x41
#define REG_INIT_CTRL     0x59
#define REG_INIT_ADDR_0   0x5B
#define REG_INIT_DATA     0x5E
#define REG_PWR_CONF      0x7C
#define REG_PWR_CTRL      0x7D
#define REG_CMD           0x7E
#define CMD_SOFT_RESET    0xB6
#define CHUNK             256
#define REUSE_MS          10

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static bool s_tried, s_ok;
static int16_t s_acc[3];
static TickType_t s_read_at;
static bool s_have;
static int s_fails;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t wr8(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, 2, 50);
}

static bool start(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus || i2c_master_probe(bus, BMI_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no accelerometer at 0x%02X", BMI_ADDR);
        return false;
    }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BMI_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;

    uint8_t id = 0, st = 0;
    if (rd(REG_CHIP_ID, &id, 1) != ESP_OK || id != 0x24) {
        ESP_LOGW(TAG, "the chip at 0x%02X is not a BMI270 (id %02X)", BMI_ADDR, id);
        goto fail;
    }
    TickType_t t0 = xTaskGetTickCount();
    wr8(REG_CMD, CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (wr8(REG_PWR_CONF, 0x00) != ESP_OK) goto fail;          /* advanced power save off, or the load is refused */
    vTaskDelay(pdMS_TO_TICKS(2));
    if (wr8(REG_INIT_CTRL, 0x00) != ESP_OK) goto fail;
    static uint8_t buf[1 + CHUNK];
    for (size_t at = 0; at < sizeof(bmi270_config_file); at += CHUNK) {
        size_t n = sizeof(bmi270_config_file) - at;
        if (n > CHUNK) n = CHUNK;
        uint8_t where[3] = { REG_INIT_ADDR_0, (uint8_t)((at >> 1) & 0x0F), (uint8_t)(at >> 5) };   /* counted in words */
        buf[0] = REG_INIT_DATA;
        memcpy(buf + 1, bmi270_config_file + at, n);
        if (i2c_master_transmit(s_dev, where, 3, 50) != ESP_OK || i2c_master_transmit(s_dev, buf, 1 + n, 100) != ESP_OK) {
            ESP_LOGW(TAG, "configuration load failed at byte %u", (unsigned)at);
            goto fail;
        }
    }
    if (wr8(REG_INIT_CTRL, 0x01) != ESP_OK) goto fail;
    for (int i = 0; i < 40 && (st & 0x0F) != 0x01; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
        rd(REG_INTERNAL_STAT, &st, 1);
    }
    if ((st & 0x0F) != 0x01) {
        ESP_LOGW(TAG, "the BMI270 did not take its configuration (status %02X)", st);
        goto fail;
    }
    if (wr8(REG_ACC_CONF, 0xA8) != ESP_OK ||                   /* 100 Hz, normal filter, performance mode */
        wr8(REG_ACC_RANGE, 0x02) != ESP_OK ||                  /* +-8 g */
        wr8(REG_PWR_CTRL, 0x04) != ESP_OK) goto fail;          /* accelerometer on; gyroscope, aux and temperature off */
    for (int i = 0; i < 50; i++) {                             /* until the first sample is there (STATUS, drdy_acc) */
        vTaskDelay(pdMS_TO_TICKS(2));
        if (rd(REG_STATUS, &st, 1) == ESP_OK && (st & 0x80)) break;
    }
    ESP_LOGI(TAG, "BMI270 accelerometer up in %d ms", (int)pdTICKS_TO_MS(xTaskGetTickCount() - t0));
    return true;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

/* axis 0..2: the reading moved up by 32768, or -1 if there is no accelerometer */
int board_imu_read(int axis)
{
    if (axis < 0 || axis > 2) return -1;
    if (!s_lock) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t made = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&mux);
        if (!s_lock) { s_lock = made; made = NULL; }
        portEXIT_CRITICAL(&mux);
        if (made) vSemaphoreDelete(made);
        if (!s_lock) return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_tried) {
        s_tried = true;
        s_ok = start();
    }
    int out = -1;
    if (s_ok) {
        TickType_t now = xTaskGetTickCount();
        if (!s_have || now - s_read_at >= pdMS_TO_TICKS(REUSE_MS)) {
            uint8_t raw[6];
            if (rd(REG_ACC_X_LSB, raw, 6) == ESP_OK) {
                for (int i = 0; i < 3; i++) s_acc[i] = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
                s_read_at = now;
                s_have = true;
                s_fails = 0;
            } else if (++s_fails >= 20) {
                ESP_LOGW(TAG, "the accelerometer stopped answering");
                s_ok = false;
            }
        }
        if (s_ok && s_have) out = (int)s_acc[axis] + 32768;
    }
    xSemaphoreGive(s_lock);
    return out;
}

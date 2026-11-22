/*
 * The Tab5's battery: a 2-cell Li-ion pack measured by an INA226 (0x41, 5 mOhm shunt) on the board's I2C bus, and
 * a charger switched by I/O expander 2 P7 (high = charging allowed) that reports on P6 (high = charging). As
 * M5Unified does it (Power_Class, board_M5Tab5): the shunt reads negative while the pack charges, so the current
 * is turned round here, positive = charging. Voltages are given per cell as well, the shape bsp_battery_mv's
 * callers expect of a single Li-ion cell.
 *
 * Nothing set P7 before, so the Tab5 never charged under the launcher: bsp_power_init turns charging on at start.
 */
#include "board_internal.h"
#include "board_pins.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_power";

#define INA_ADDR      0x41
#define INA_CONFIG    0x00
#define INA_SHUNT     0x01      /* 2.5 uV a count, signed */
#define INA_BUS       0x02      /* 1.25 mV a count */
#define INA_SHUNT_MOHM 5
#define IOX_IN        0x0F
#define IOX_OUT       0x05
#define IOX_CHG_EN    (1 << 7)
#define IOX_CHG_STAT  (1 << 6)

static i2c_master_dev_handle_t s_ina, s_iox;
static SemaphoreHandle_t s_lock;
static bool s_charge_on;

static bool rd16(uint8_t reg, int16_t *v)
{
    uint8_t b[2];
    if (i2c_master_transmit_receive(s_ina, &reg, 1, b, 2, 50) != ESP_OK) return false;
    *v = (int16_t)(b[0] << 8 | b[1]);
    return true;
}

static bool iox_rd(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_iox, &reg, 1, v, 1, 50) == ESP_OK;
}

esp_err_t bsp_set_charge(bool on)
{
    if (!s_iox) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t out = 0;
    esp_err_t e = ESP_FAIL;
    if (iox_rd(IOX_OUT, &out)) {
        uint8_t b[2] = { IOX_OUT, (uint8_t)(on ? out | IOX_CHG_EN : out & ~IOX_CHG_EN) };
        e = i2c_master_transmit(s_iox, b, 2, 50);
        if (e == ESP_OK) s_charge_on = on;
    }
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t bsp_power_read(bsp_power_t *out)
{
    if (!s_ina) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int16_t sh = 0, bus = 0;
    uint8_t in = 0;
    bool ok = rd16(INA_SHUNT, &sh) && rd16(INA_BUS, &bus);
    bool st = s_iox && iox_rd(IOX_IN, &in);
    xSemaphoreGive(s_lock);
    if (!ok) return ESP_FAIL;
    out->pack_mv = (int)((int32_t)(uint16_t)bus * 125 / 100);
    out->cell_mv = out->pack_mv / 2;
    out->ma = -(int)((int32_t)sh * 25 / 10 / INA_SHUNT_MOHM);     /* uV / mOhm = mA; positive = charging */
    out->charging = st && (in & IOX_CHG_STAT);
    out->charge_enabled = s_charge_on;
    return ESP_OK;
}

int bsp_battery_mv(void)
{
    bsp_power_t p;
    return bsp_power_read(&p) == ESP_OK ? p.cell_mv : -1;
}

esp_err_t bsp_power_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus || s_ina) return bus ? ESP_OK : ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = INA_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_probe(bus, INA_ADDR, 50) != ESP_OK || i2c_master_bus_add_device(bus, &cfg, &s_ina) != ESP_OK) {
        s_ina = NULL;
        ESP_LOGW(TAG, "no INA226 at 0x%02X: no battery reading", INA_ADDR);
    } else {
        /* averages of 16, 1.1 ms conversions, shunt and bus continuous (M5Unified's settings) */
        uint8_t b[3] = { INA_CONFIG, 0x45, 0x27 };
        i2c_master_transmit(s_ina, b, 3, 50);
        board_set_cap(BSP_CAP_BATTERY);
    }
    cfg.device_address = BOARD_IOX2_ADDR;
    if (i2c_master_bus_add_device(bus, &cfg, &s_iox) != ESP_OK) s_iox = NULL;
    if (s_iox) {
        bsp_power_t before = { 0 }, after = { 0 };
        bsp_power_read(&before);
        bsp_set_charge(true);
        board_set_cap(BSP_CAP_CHARGE);
        vTaskDelay(pdMS_TO_TICKS(300));
        bsp_power_read(&after);
        ESP_LOGI(TAG, "battery %d mV (%d a cell): %d mA with charging off as booted, %d mA with it on%s", after.pack_mv,
                 after.cell_mv, before.ma, after.ma, after.charging ? " (the charger says charging)" : "");
    }
    return ESP_OK;
}

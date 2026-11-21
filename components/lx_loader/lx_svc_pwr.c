/*
 * PWR: the battery and its charging for apps (papp_abi.h). The board does the work (bsp_power_read,
 * bsp_set_charge); boards without them get the weak versions here. Charging an app stops is turned back on
 * at the end of its run.
 */
#include "lx_services.h"
#include "bsp.h"

#include "esp_log.h"

static const char *TAG = "lx_pwr";

static bool s_stopped;           /* the running app stopped charging */

__attribute__((weak)) esp_err_t bsp_power_read(bsp_power_t *out) { (void)out; return ESP_ERR_NOT_SUPPORTED; }
__attribute__((weak)) esp_err_t bsp_set_charge(bool on) { (void)on; return ESP_ERR_NOT_SUPPORTED; }

static int pwr_read(papp_power_t *out)
{
    if (!out) return PAPP_E_ARG;
    SVC_ENTER();
    bsp_power_t p;
    esp_err_t e = bsp_power_read(&p);
    SVC_LEAVE();
    if (e != ESP_OK) return PAPP_E_UNSUPPORTED;
    out->pack_mv = p.pack_mv;
    out->cell_mv = p.cell_mv;
    out->ma = p.ma;
    int pct = (p.cell_mv - 3300) * 100 / (4150 - 3300);   /* M5Unified's Li-ion curve, by voltage only */
    out->percent = (int8_t)(p.cell_mv < 2800 ? -1 : pct < 0 ? 0 : pct > 100 ? 100 : pct);
    out->charging = p.charging;
    out->charge_enabled = p.charge_enabled;
    out->can_switch = (bsp_info()->caps & BSP_CAP_CHARGE) != 0;
    return PAPP_E_OK;
}

static int pwr_set_charge(bool on)
{
    SVC_ENTER();
    esp_err_t e = bsp_set_charge(on);
    if (e == ESP_OK) {
        s_stopped = !on;
        ESP_LOGI(TAG, "the app turned charging %s", on ? "on" : "off");
    }
    SVC_LEAVE();
    return e == ESP_OK ? PAPP_E_OK : PAPP_E_UNSUPPORTED;
}

const papp_if_pwr_t lx_if_pwr = {
    .hdr = { PAPP_IF_PWR, PAPP_IF_PWR_VER, sizeof(papp_if_pwr_t) },
    .read = pwr_read,
    .set_charge = pwr_set_charge,
};

void lx_svc_pwr_end_run(void)
{
    if (!s_stopped) return;
    s_stopped = false;
    if (bsp_set_charge(true) == ESP_OK) ESP_LOGI(TAG, "charging back on after the app");
}

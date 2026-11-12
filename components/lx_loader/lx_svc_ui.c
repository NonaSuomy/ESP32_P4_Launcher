/*
 * UI interface: launcher-drawn browser and message box (lx_ui). The calls run
 * on the app's task; lx_ui brackets each locked stretch with SVC_ENTER/LEAVE
 * (guard below), so a stop request ends them (CANCEL) and a forced stop never
 * catches them holding the display or a file.
 *
 * Also the in-game overlay: HOME asks for it, and the app's next present opens
 * it on the app's own task, so the game is frozen while it shows.
 */
#include "lx_services.h"
#include <string.h>
#include "bsp.h"
#include "esp_heap_caps.h"
#include "lx_audio.h"
#include "lx_comp.h"
#include "lx_input.h"
#include "lx_ui.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile int s_ui_depth;         /* app is inside a UI call */

static void guard_enter(void) { SVC_ENTER(); }
static void guard_leave(void) { SVC_LEAVE(); }

static int ui_pick_file(const papp_pick_cfg_t *cfg, papp_pick_result_t *out)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    s_ui_depth++;
    int r = lx_ui_pick_file(cfg, out, &lx_svc_exit_flag);
    s_ui_depth--;
    return r;
}

static void ui_message(const char *title, const char *text)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    s_ui_depth++;
    lx_ui_message(title, text, &lx_svc_exit_flag);
    s_ui_depth--;
}

static int ui_menu(const char *title, const char *const *items, int n, int initial)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    s_ui_depth++;
    int r = lx_ui_menu(title, items, n, initial, &lx_svc_exit_flag);
    s_ui_depth--;
    return r;
}

static int ui_volume(void)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    s_ui_depth++;
    int r = lx_ui_volume(&lx_svc_exit_flag);
    s_ui_depth--;
    return r;
}

static int ui_wifi(void)
{
    int e = lx_svc_net_hold();          /* the radio for the run, released at teardown */
    if (e < 0) return e;
    lx_ui_set_guard(guard_enter, guard_leave);
    s_ui_depth++;
    int r = lx_ui_wifi(&lx_svc_exit_flag);
    s_ui_depth--;
    return r < 0 && lx_svc_exit_flag ? PAPP_E_EXIT : r;
}

const papp_if_ui_t lx_if_ui = {
    .hdr = { PAPP_IF_UI, PAPP_IF_UI_VER, sizeof(papp_if_ui_t) },
    .pick_file = ui_pick_file,
    .message   = ui_message,
    .menu      = ui_menu,
    .volume    = ui_volume,
    .wifi      = ui_wifi,
};

/* ── In-game overlay ───────────────────────────────────────────────────── */

static volatile bool s_ovl_req, s_ovl_open;
static int s_shot_in;                   /* presents until the screenshot */

void lx_overlay_reset(void)
{
    s_ovl_req = s_ovl_open = false;
    s_shot_in = 0;
}

bool lx_app_overlay_request(void)
{
    if (lx_app_state() != LX_APP_RUNNING) return false;
    if (s_ovl_open) return true;        /* already showing */
    /* In a launcher screen, or asked before and the app never presented: the
     * menu can't come up, so HOME quits as it always did */
    if (s_ui_depth || s_ovl_req) return false;
    s_ovl_req = true;
    return true;
}

/* So A, used to pick, doesn't reach the game as a press */
static void wait_release(void)
{
    int64_t until = esp_timer_get_time() + 500000;
    papp_gamepad_t g;
    while (esp_timer_get_time() < until && lx_input_gamepad(0, &g) && g.buttons) vTaskDelay(pdMS_TO_TICKS(10));
    lx_input_flush();
}

void lx_overlay_poll(void)
{
    if (s_shot_in && --s_shot_in == 0) lx_comp_screenshot(NULL, 0);    /* the game's frame is up again */
    if (!s_ovl_req || lx_svc_exit_flag) return;
    s_ovl_open = true;
    s_ovl_req = false;
    lx_audio_hold(true);
    lx_ui_set_guard(guard_enter, guard_leave);
    /* The menu dims what is on screen: put the game's frame back before it
     * opens again, or each round dims it further */
    const bsp_info_t *b = bsp_info();
    size_t bytes = (size_t)b->display_w * b->display_h * 2;
    uint16_t *game = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
    if (game) lx_comp_copy_visible(game);
    static const char *const items[] = { "Resume", "Volume", "Screenshot", "Quit to home" };
    int pick = 0;
    for (;;) {
        pick = lx_ui_menu("Paused", items, 4, pick, &lx_svc_exit_flag);
        if (pick != 1 || lx_svc_exit_flag) break;
        lx_ui_volume(&lx_svc_exit_flag);    /* then back to the menu */
        SVC_ENTER();
        uint16_t *fb = game ? lx_comp_takeover_begin() : NULL;
        if (fb) {
            memcpy(fb, game, bytes);
            lx_comp_launcher_end();
        }
        SVC_LEAVE();
    }
    heap_caps_free(game);
    if (pick == 2) s_shot_in = 2;       /* without the menu in it */
    bool hold_audio_until_exit = false;
    if (pick == 3)
        hold_audio_until_exit = lx_app_stop(2000) == ESP_OK;
    /* An external HOME/STOP can also dismiss this menu. In either case the
     * app may still be writing audio while its tasks and streams are torn
     * down, so leave the mixer held until the app-exit callback runs. */
    if (lx_svc_exit_flag) hold_audio_until_exit = true;
    wait_release();
    if (!hold_audio_until_exit) lx_audio_hold(false);
    s_ovl_open = false;
}

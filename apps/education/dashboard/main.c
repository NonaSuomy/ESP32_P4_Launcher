/*
 * Sensor Dashboard (education): measure the world around the board, graph it, save it.
 *
 * The board has no thermometer, but it has radios and memory, and they are sensors too:
 *   - Wi-Fi signal: how strong the network you joined arrives, in dBm. -40 is next to the router,
 *     -80 is the edge of the house; every 10 less is 10 times weaker.
 *   - Wi-Fi networks: how many the board hears around it.
 *   - Bluetooth devices: phones, watches, headphones, TVs that are advertising nearby.
 *   - Free memory: what apps still have left (internal SRAM and PSRAM).
 * A helper thread measures them in turn (a scan takes a few seconds, and the screen must not
 * wait for it); the screen takes each new sample, adds it to the graph of the chosen value and,
 * while Record is on, writes a line to a CSV file in /sd/logs: open it in a spreadsheet.
 *
 * Built with LVGL (see the LVGL Demo for how an LVGL app is put together).
 * Touch a tile to graph it. Pad: LEFT / RIGHT pick the value, A record, X exits.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "papp/papp.h"
#include "lvgl.h"

PAPP_MANIFEST(.id = "edu.dashboard", .name = "Sensor Dashboard", .version = PAPP_APP_VERSION, .main_stack = 32768);

#define W 800
#define H 480
#define POINTS 60                       /* samples on the graph */
#define LOG_DIR "/sd/logs"

enum { M_RSSI, M_NETS, M_BLE, M_SRAM, M_PSRAM, METRICS };
static const char *const m_name[METRICS] = { "Wi-Fi signal", "Wi-Fi networks", "Bluetooth", "Free SRAM", "Free PSRAM" };
static const char *const m_unit[METRICS] = { "dBm", "nearby", "devices", "KB", "MB" };
static const int32_t m_min[METRICS] = { -100, 0, 0, 0, 0 };
static const lv_palette_t m_color[METRICS] = { LV_PALETTE_CYAN, LV_PALETTE_LIGHT_BLUE, LV_PALETTE_INDIGO,
                                               LV_PALETTE_GREEN, LV_PALETTE_AMBER };

/* ── The measuring thread ──────────────────────────────────────────────── */

typedef struct {
    int32_t v[METRICS];
    bool valid[METRICS];
} sample_t;

static papp_mutex_t *s_mx;
static sample_t s_latest;
static volatile int s_seq;              /* +1 at every new sample */
static volatile bool s_stop;
static char s_activity[48] = "Starting";    /* what the thread is doing now */

static void set_activity(const char *a)
{
    papp_mutex_lock(s_mx);
    snprintf(s_activity, sizeof(s_activity), "%s", a);
    papp_mutex_unlock(s_mx);
}

static int measure_thread(void *arg)
{
    (void)arg;
    const papp_if_net_t *net = papp_net();
    const papp_if_ble_t *ble = papp_ble();
    bool ble_ok = ble && ble->available();
    if (net) {
        set_activity("Joining Wi-Fi");
        net->up(8000);                             /* the saved network, if there is one */
    }
    while (!s_stop && !papp_exit_requested()) {
        sample_t smp = { 0 };
        if (net) {                                 /* Wi-Fi: signal of our network, networks around */
            set_activity("Wi-Fi scan");
            papp_net_info_t in;
            net->info(&in);
            if (in.state == PAPP_NET_CONNECTED && in.rssi) {
                smp.v[M_RSSI] = in.rssi;
                smp.valid[M_RSSI] = true;
            }
            static papp_net_ap_t aps[40];
            int n = net->scan(aps, 40, 6000);
            if (n >= 0) {
                smp.v[M_NETS] = n;
                smp.valid[M_NETS] = true;
            }
        }
        if (ble_ok && !s_stop) {                   /* Bluetooth: different addresses heard in 3 s */
            set_activity("Bluetooth scan");
            papp_ble_scan_cfg_t sc = { .active = 0, .filter_duplicates = 1, .duration_ms = 3000 };
            if (ble->scan_start(&sc) == 0) {
                static papp_ble_addr_t seen[64];
                int n = 0;
                papp_ble_event_t e;
                while (!s_stop && ble->wait_event(&e, 4000)) {
                    if (e.type == PAPP_BLE_EV_SCAN_DONE) break;
                    if (e.type != PAPP_BLE_EV_ADV) continue;
                    bool known = false;
                    for (int i = 0; i < n && !known; i++) known = !memcmp(&seen[i], &e.addr, sizeof(e.addr));
                    if (!known && n < 64) seen[n++] = e.addr;
                }
                ble->scan_stop();
                smp.v[M_BLE] = n;
                smp.valid[M_BLE] = true;
            }
        }
        smp.v[M_SRAM] = (int32_t)(papp_mem_free_size(PAPP_MEM_FAST) / 1024);
        smp.v[M_PSRAM] = (int32_t)(papp_mem_free_size(PAPP_MEM_LARGE) / (1024 * 1024));
        smp.valid[M_SRAM] = smp.valid[M_PSRAM] = true;
        papp_mutex_lock(s_mx);
        s_latest = smp;
        s_seq++;
        papp_mutex_unlock(s_mx);
        set_activity("Waiting");
        for (int i = 0; i < 10 && !s_stop && !papp_exit_requested(); i++) papp_delay_ms(100);
    }
    return 0;
}

/* ── LVGL glue: clock, display, touch ──────────────────────────────────── */

static papp_surface_t *s_surf;
static lv_display_t *s_disp;
static bool s_quit;

static uint32_t tick_ms(void) { return (uint32_t)(papp_time_us() / 1000); }

static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    if (lv_display_flush_is_last(d)) papp_disp_present(s_surf);
    lv_display_flush_ready(d);
}

static void touch_read(lv_indev_t *in, lv_indev_data_t *data)
{
    (void)in;
    papp_touch_point_t p;
    if (papp_disp_touch(s_surf, &p, 1) > 0) {
        data->point.x = p.x;
        data->point.y = p.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ── The screen ────────────────────────────────────────────────────────── */

static lv_obj_t *s_tile[METRICS], *s_value[METRICS], *s_chart, *s_chart_title, *s_status, *s_rec_btn, *s_rec_label;
static lv_obj_t *s_axis_min, *s_axis_max;
static lv_chart_series_t *s_ser;
static int32_t s_hist[METRICS][POINTS];
static bool s_hist_ok[METRICS][POINTS];
static int s_count;                     /* samples so far */
static int s_metric = M_NETS;
static FILE *s_log;
static char s_log_name[40];
static int s_logged;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, text);
    return l;
}

/* The graph of the chosen value: its history, the range rounded out to whole steps */
static void show_chart(void)
{
    int32_t lo = 1 << 30, hi = -(1 << 30);
    int32_t *y = lv_chart_get_y_array(s_chart, s_ser);
    for (int i = 0; i < POINTS; i++) {
        y[i] = s_hist_ok[s_metric][i] ? s_hist[s_metric][i] : LV_CHART_POINT_NONE;
        if (s_hist_ok[s_metric][i]) {
            if (s_hist[s_metric][i] < lo) lo = s_hist[s_metric][i];
            if (s_hist[s_metric][i] > hi) hi = s_hist[s_metric][i];
        }
    }
    if (lo > hi) lo = hi = m_min[s_metric] < 0 ? -60 : 0;
    int32_t step = hi - lo > 200 ? 100 : hi - lo > 40 ? 20 : 5;
    lo = (lo - step) / step * step;
    hi = (hi + step + step - 1) / step * step;
    if (lo < m_min[s_metric]) lo = m_min[s_metric];
    if (m_min[s_metric] < 0 && hi > 0) hi = 0;
    if (hi <= lo) hi = lo + step;
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    lv_chart_set_series_color(s_chart, s_ser, lv_palette_main(m_color[s_metric]));
    lv_chart_refresh(s_chart);
    lv_label_set_text_fmt(s_axis_min, "%d", (int)lo);
    lv_label_set_text_fmt(s_axis_max, "%d", (int)hi);
    lv_label_set_text_fmt(s_chart_title, "%s (%s), a new sample every few seconds", m_name[s_metric], m_unit[s_metric]);
    for (int m = 0; m < METRICS; m++)
        lv_obj_set_style_border_color(s_tile[m], m == s_metric ? lv_palette_main(m_color[m]) : lv_color_hex(0x223150), 0);
}

static void pick(int m)
{
    s_metric = (m + METRICS) % METRICS;
    show_chart();
}

static void tile_clicked(lv_event_t *e) { pick((int)(intptr_t)lv_event_get_user_data(e)); }

/* Record: open a new CSV file (dash_001.csv, ...) and write a line per sample */
static void record(bool on)
{
    if (on && !s_log) {
        mkdir(LOG_DIR, 0777);
        for (int i = 1; i < 1000 && !s_log; i++) {
            char path[64];
            snprintf(s_log_name, sizeof(s_log_name), "dash_%03d.csv", i);
            snprintf(path, sizeof(path), LOG_DIR "/%s", s_log_name);
            FILE *t = fopen(path, "rb");
            if (t) { fclose(t); continue; }
            s_log = fopen(path, "w");
        }
        if (s_log) {
            fprintf(s_log, "time,wifi_dbm,wifi_networks,ble_devices,free_sram_kb,free_psram_mb\n");
            s_logged = 0;
            papp_log("recording to %s\n", s_log_name);
        }
    } else if (!on && s_log) {
        fclose(s_log);
        s_log = NULL;
        papp_log("%s: %d lines\n", s_log_name, s_logged);
    }
    lv_label_set_text(s_rec_label, s_log ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_SAVE "  Record");
    lv_obj_set_style_bg_color(s_rec_btn, s_log ? lv_palette_main(LV_PALETTE_RED) : lv_palette_main(LV_PALETTE_BLUE), 0);
}

static void rec_clicked(lv_event_t *e)
{
    (void)e;
    record(!s_log);
}

static void close_clicked(lv_event_t *e)
{
    (void)e;
    s_quit = true;
}

static void make_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A1020), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = label(scr, &lv_font_montserrat_28, lv_color_white(), "Sensor Dashboard");
    lv_obj_set_pos(t, 20, 14);
    s_status = label(scr, &lv_font_montserrat_14, lv_color_hex(0x8A9AB8), "");
    lv_obj_set_pos(s_status, 22, 52);

    s_rec_btn = lv_button_create(scr);
    lv_obj_set_size(s_rec_btn, 140, 46);
    lv_obj_align(s_rec_btn, LV_ALIGN_TOP_RIGHT, -76, 12);
    s_rec_label = label(s_rec_btn, &lv_font_montserrat_20, lv_color_white(), "");
    lv_obj_center(s_rec_label);
    lv_obj_add_event_cb(s_rec_btn, rec_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *x = lv_button_create(scr);
    lv_obj_set_size(x, 48, 46);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x223150), 0);
    lv_obj_t *xl = label(x, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_CLOSE);
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, close_clicked, LV_EVENT_CLICKED, NULL);

    for (int m = 0; m < METRICS; m++) {           /* the tiles: name, big number, unit */
        lv_obj_t *tile = lv_obj_create(scr);
        s_tile[m] = tile;
        lv_obj_set_size(tile, 146, 104);
        lv_obj_set_pos(tile, 20 + m * 154, 80);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x131D33), 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_pad_all(tile, 10, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)m);
        lv_obj_t *n = label(tile, &lv_font_montserrat_14, lv_color_hex(0x8A9AB8), m_name[m]);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        s_value[m] = label(tile, &lv_font_montserrat_40, lv_palette_lighten(m_color[m], 2), "-");
        lv_obj_align(s_value[m], LV_ALIGN_LEFT_MID, 0, 6);
        lv_obj_t *u = label(tile, &lv_font_montserrat_14, lv_color_hex(0x5A6A88), m_unit[m]);
        lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }

    s_chart_title = label(scr, &lv_font_montserrat_20, lv_color_hex(0xC8D4EC), "");
    lv_obj_set_pos(s_chart_title, 22, 198);
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 700, 236);
    lv_obj_set_pos(s_chart, 76, 230);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x0E1628), 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(0x223150), 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x1B2740), LV_PART_MAIN);     /* grid */
    lv_obj_set_style_line_width(s_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(s_chart, 5, 5, LV_PART_INDICATOR);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, POINTS);
    lv_chart_set_div_line_count(s_chart, 5, 7);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
    s_ser = lv_chart_add_series(s_chart, lv_palette_main(m_color[s_metric]), LV_CHART_AXIS_PRIMARY_Y);
    s_axis_max = label(scr, &lv_font_montserrat_14, lv_color_hex(0x8A9AB8), "");
    lv_obj_set_width(s_axis_max, 48);
    lv_obj_set_style_text_align(s_axis_max, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_axis_max, 20, 234);
    s_axis_min = label(scr, &lv_font_montserrat_14, lv_color_hex(0x8A9AB8), "");
    lv_obj_set_width(s_axis_min, 48);
    lv_obj_set_style_text_align(s_axis_min, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_axis_min, 20, 446);
    record(false);
    show_chart();
}

/* A new sample: tiles, history, graph, CSV line */
static void take_sample(const sample_t *smp)
{
    for (int m = 0; m < METRICS; m++) {
        memmove(&s_hist[m][0], &s_hist[m][1], (POINTS - 1) * sizeof(int32_t));
        memmove(&s_hist_ok[m][0], &s_hist_ok[m][1], (POINTS - 1) * sizeof(bool));
        s_hist[m][POINTS - 1] = smp->v[m];
        s_hist_ok[m][POINTS - 1] = smp->valid[m];
        if (smp->valid[m]) lv_label_set_text_fmt(s_value[m], "%d", (int)smp->v[m]);
        else lv_label_set_text(s_value[m], "-");
    }
    s_count++;
    show_chart();
    if (s_log) {
        papp_datetime_t dt;
        char when[40] = "";
        if (papp_local_time(&dt))
            snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        else
            snprintf(when, sizeof(when), "%lld", (long long)(papp_time_us() / 1000000));
        fprintf(s_log, "%s", when);
        for (int m = 0; m < METRICS; m++) {
            if (smp->valid[m]) fprintf(s_log, ",%d", (int)smp->v[m]);
            else fprintf(s_log, ",");
        }
        fprintf(s_log, "\n");
        fflush(s_log);
        s_logged++;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    s_surf = papp_disp_surface_create(&cfg);
    s_mx = papp_mutex_create(false);
    if (!s_surf || !s_mx) return 1;
    int stride;
    void *fb = papp_disp_lock(s_surf, &stride);

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(W, H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, fb, NULL, (uint32_t)stride * H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush);
    lv_display_set_theme(s_disp, lv_theme_default_init(s_disp, lv_palette_main(LV_PALETTE_BLUE),
                                                       lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT));
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    make_ui();

    papp_thread_cfg_t tc = { .name = "measure", .stack_size = 8192, .prio = PAPP_PRIO_NORMAL, .core = -1 };
    papp_thread_t *th = papp_thread_create(measure_thread, NULL, &tc);
    if (!th) return 1;

    uint32_t prev = 0xFFFFFFFFu;
    int seen = 0;
    int64_t status_at = 0;
    while (!papp_exit_requested() && !s_quit) {
        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_X) break;
        if (press & PAPP_BTN_LEFT) pick(s_metric - 1);
        if (press & PAPP_BTN_RIGHT) pick(s_metric + 1);
        if (press & PAPP_BTN_A) record(!s_log);

        sample_t smp;
        bool fresh = false;
        char act[48];
        papp_mutex_lock(s_mx);
        if (s_seq != seen) {
            seen = s_seq;
            smp = s_latest;
            fresh = true;
        }
        snprintf(act, sizeof(act), "%s", s_activity);
        papp_mutex_unlock(s_mx);
        if (fresh) take_sample(&smp);
        int64_t now = papp_time_us();
        if (fresh || now >= status_at) {
            status_at = now + 500000;
            char st[128];
            int n = snprintf(st, sizeof(st), "%d samples   Now: %s", s_count, act);
            if (s_log) snprintf(st + n, sizeof(st) - n, "   Recording %s (%d lines)", s_log_name, s_logged);
            lv_label_set_text(s_status, st);
        }

        uint32_t wait = lv_timer_handler();
        papp_delay_ms(wait > 20 ? 20 : wait ? wait : 1);
    }
    s_stop = true;
    record(false);
    papp_thread_join(th, PAPP_WAIT_FOREVER, NULL);
    papp_mutex_destroy(s_mx);
    lv_deinit();
    papp_disp_surface_destroy(s_surf);
    return 0;
}

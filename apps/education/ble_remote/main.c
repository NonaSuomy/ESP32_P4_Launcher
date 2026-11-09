/*
 * BLE Remote — the Gution ESP32-P4 BLE demo as an app: a peripheral named "gution-p4" with
 *   Control service 4f3a0001-9d2b-4c7e-a1f5-8b6d3e0c7a21
 *     mode  4f3a0002  uint8  0=Idle 1=Run 2=Test   read / write / notify
 *     level 4f3a0003  uint8  0..100                read / write / notify
 *     led   4f3a0004  uint8  0 or 1                read / write / notify
 *     msg   4f3a0005  utf8   up to 32 bytes        read / write / notify
 *   Nordic UART service: a console ("help", "status", "mode N", "level N", "led on|off",
 *     "msg text", "tab 1|2"), the same as the Gution firmware, so its PC tools
 *     (GutionTerminal, ble_client.py) and any BLE phone app work with it.
 *
 * Screen: Control (mode, level, LED, message; changes reach the connected devices at once)
 * and BLE (connection, a log of what arrives and what is sent, Send / Hello / Clear;
 * a USB keyboard types the line to send).
 * Pad: L1 / R1 tab, UP / DOWN mode, LEFT / RIGHT level, A LED, START menu.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "papp/papp.h"
#include "edu_ui.h"

/* An education app: built into SDcard/apps/Education (apps/education/build.py), never into the Store */
PAPP_MANIFEST(.id = "edu.bleremote", .name = "BLE Remote", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define CTRL_SVC   "4f3a0001-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_MODE  "4f3a0002-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_LEVEL "4f3a0003-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_LED   "4f3a0004-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_MSG   "4f3a0005-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define NUS_SVC    "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX     "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define NAME       "gution-p4"

enum { TAB_CONTROL, TAB_BLE };
enum { H_SEND = 1, H_HELLO, H_CLEAR };

static const papp_if_ble_t *ble;
static uint16_t h_mode, h_level, h_led, h_msg, h_rx, h_tx;
static int      s_tab, s_conns;
static uint16_t s_mtu[PAPP_BLE_MAX_CONN];
static papp_ble_addr_t s_peer[PAPP_BLE_MAX_CONN];
static bool     s_live[PAPP_BLE_MAX_CONN];
static char     s_rxl[PAPP_BLE_MAX_CONN][128];    /* console line being assembled, per link */
static int      s_rxn[PAPP_BLE_MAX_CONN];
static papp_ble_adv_cfg_t s_adv;
static uint8_t  s_adv_d[31], s_rsp_d[31];
static char     s_status[96] = "Starting Bluetooth...";
static bool     s_ok, s_quit;
static char     s_type[64];                   /* typed on a USB keyboard */
static int      s_typen;

static const char *addr_str(const papp_ble_addr_t *a)
{
    static char b[24];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", a->a[5], a->a[4], a->a[3], a->a[2], a->a[1], a->a[0]);
    return b;
}

/* ── BLE ───────────────────────────────────────────────────────────────── */

static papp_ble_uuid_t U(const char *s)
{
    papp_ble_uuid_t u;
    papp_ble_uuid_parse(s, &u);
    return u;
}

static void send_text(papp_ble_conn_t c, const char *text)
{
    int n = (int)strlen(text);
    ui_log_bytes(1, text, n);
    for (int k = 0; k < PAPP_BLE_MAX_CONN; k++) {
        if (!s_live[k] || (c != PAPP_BLE_CONN_ALL && c != k)) continue;
        int chunk = (s_mtu[k] > 23 ? s_mtu[k] : 23) - 3;
        for (int i = 0; i < n; i += chunk)
            if (ble->notify((papp_ble_conn_t)k, h_tx, text + i, n - i < chunk ? n - i : chunk)) break;
    }
}

static void reply(papp_ble_conn_t c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(papp_ble_conn_t c, const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    send_text(c, b);
}

/* Values changed (screen, pad, console or a direct write): tell the subscribers */
static void publish(int what)
{
    if (!s_ok) return;
    if (what & REMOTE_MODE) ble->notify(PAPP_BLE_CONN_ALL, h_mode, &remote.mode, 1);
    if (what & REMOTE_LEVEL) ble->notify(PAPP_BLE_CONN_ALL, h_level, &remote.level, 1);
    if (what & REMOTE_LED) ble->notify(PAPP_BLE_CONN_ALL, h_led, &remote.led, 1);
    if (what & REMOTE_MSG) ble->notify(PAPP_BLE_CONN_ALL, h_msg, remote.msg, strlen(remote.msg));
}

static void changed(int what, const char *who)
{
    remote_changed(what, who);
    publish(what);
}

static void command(papp_ble_conn_t c, char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    char *arg = line + strcspn(line, " ");
    if (*arg) *arg++ = 0;
    while (*arg == ' ') arg++;

    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        reply(c, "commands:\r\n"
                 "  status         show everything\r\n"
                 "  mode <0|1|2>   Idle / Run / Test\r\n"
                 "  level <0-100>\r\n"
                 "  led <on|off>\r\n"
                 "  msg <text>     shown on the device\r\n"
                 "  tab <1|2>      switch the screen\r\n"
                 "  help\r\n");
    } else if (!strcmp(line, "status")) {
        reply(c, "mode  %d (%s)\r\nlevel %d\r\nled   %s\r\nmsg   %s\r\nwifi  off (paused while the app runs)\r\n"
                 "fw    BLE Remote %s\r\n",
              remote.mode, remote_mode_name[remote.mode], remote.level, remote.led ? "on" : "off", remote.msg,
              PAPP_APP_VERSION);
    } else if (!strcmp(line, "mode")) {
        int v = atoi(arg);
        if (!*arg || v < 0 || v > 2) { reply(c, "mode must be 0..2\r\n"); return; }
        remote.mode = (uint8_t)v;
        changed(REMOTE_MODE, "console");
        reply(c, "ok\r\n");
    } else if (!strcmp(line, "level")) {
        int v = atoi(arg);
        if (!*arg || v < 0 || v > 100) { reply(c, "level must be 0..100\r\n"); return; }
        remote.level = (uint8_t)v;
        changed(REMOTE_LEVEL, "console");
        reply(c, "ok\r\n");
    } else if (!strcmp(line, "led")) {
        if (!strcmp(arg, "on") || !strcmp(arg, "1")) remote.led = 1;
        else if (!strcmp(arg, "off") || !strcmp(arg, "0")) remote.led = 0;
        else { reply(c, "led takes on or off\r\n"); return; }
        changed(REMOTE_LED, "console");
        reply(c, "ok\r\n");
    } else if (!strcmp(line, "msg")) {
        snprintf(remote.msg, sizeof(remote.msg), "%s", arg);
        changed(REMOTE_MSG, "console");
        reply(c, "ok\r\n");
    } else if (!strcmp(line, "tab")) {
        int v = atoi(arg);
        if (v != 1 && v != 2) { reply(c, "tab must be 1 or 2\r\n"); return; }
        s_tab = v == 1 ? TAB_CONTROL : TAB_BLE;
        reply(c, "ok\r\n");
    } else {
        reply(c, "unknown: %s (try help)\r\n", line);
    }
}

static void console_bytes(papp_ble_conn_t c, const uint8_t *p, int n)
{
    ui_log_bytes(0, (const char *)p, n);
    if (c >= PAPP_BLE_MAX_CONN) return;
    for (int i = 0; i < n; i++) {
        if (p[i] == '\r' || p[i] == '\n') {
            if (!s_rxn[c]) continue;
            s_rxl[c][s_rxn[c]] = 0;
            s_rxn[c] = 0;
            command(c, s_rxl[c]);
        } else if (s_rxn[c] < (int)sizeof(s_rxl[c]) - 1) {
            s_rxl[c][s_rxn[c]++] = (char)p[i];
        } else {
            s_rxn[c] = 0;
            reply(c, "line too long\r\n");
        }
    }
}

static void update_status(void)
{
    if (!s_ok) return;
    if (!s_conns) snprintf(s_status, sizeof(s_status), "Advertising as \"" NAME "\" - waiting for a connection");
    else {
        int k = 0;
        while (k < PAPP_BLE_MAX_CONN && !s_live[k]) k++;
        if (s_conns == 1)
            snprintf(s_status, sizeof(s_status), "Connected: %s, MTU %u", addr_str(&s_peer[k]), s_mtu[k]);
        else
            snprintf(s_status, sizeof(s_status), "%d devices connected", s_conns);
    }
}

static bool ble_start(void)
{
    ble = papp_ble();
    if (!ble) {
        snprintf(s_status, sizeof(s_status), "No Bluetooth on this board or launcher");
        return false;
    }
    if (!ble->available()) {
        snprintf(s_status, sizeof(s_status), "Bluetooth did not start (Wi-Fi chip firmware?)");
        return false;
    }
    static papp_ble_chr_t ctrl[4], nus[2];
    ctrl[0] = (papp_ble_chr_t){ U(CTRL_MODE),  PAPP_BLE_READ | PAPP_BLE_WRITE | PAPP_BLE_NOTIFY, 1, &h_mode };
    ctrl[1] = (papp_ble_chr_t){ U(CTRL_LEVEL), PAPP_BLE_READ | PAPP_BLE_WRITE | PAPP_BLE_NOTIFY, 1, &h_level };
    ctrl[2] = (papp_ble_chr_t){ U(CTRL_LED),   PAPP_BLE_READ | PAPP_BLE_WRITE | PAPP_BLE_NOTIFY, 1, &h_led };
    ctrl[3] = (papp_ble_chr_t){ U(CTRL_MSG),   PAPP_BLE_READ | PAPP_BLE_WRITE | PAPP_BLE_NOTIFY, REMOTE_MSG_MAX,
                                &h_msg };
    nus[0] = (papp_ble_chr_t){ U(NUS_RX), PAPP_BLE_WRITE | PAPP_BLE_WRITE_NR, 244, &h_rx };
    nus[1] = (papp_ble_chr_t){ U(NUS_TX), PAPP_BLE_NOTIFY, 244, &h_tx };
    papp_ble_svc_t svcs[2] = { { U(CTRL_SVC), ctrl, 4 }, { U(NUS_SVC), nus, 2 } };

    ble->set_name(NAME);
    if (ble->gatt_register(svcs, 2)) {
        snprintf(s_status, sizeof(s_status), "Could not register the services");
        return false;
    }
    ble->set_value(h_mode, &remote.mode, 1);
    ble->set_value(h_level, &remote.level, 1);
    ble->set_value(h_led, &remote.led, 1);
    ble->set_value(h_msg, remote.msg, strlen(remote.msg));

    int an = 0, rn = 0;
    papp_ble_ad_flags(s_adv_d, &an);
    papp_ble_ad_name(s_adv_d, &an, NAME);
    papp_ble_uuid_t cu = U(CTRL_SVC);
    papp_ble_ad_uuid(s_rsp_d, &rn, &cu);
    s_adv = (papp_ble_adv_cfg_t){ .adv = s_adv_d, .adv_len = (uint8_t)an, .rsp = s_rsp_d, .rsp_len = (uint8_t)rn,
                                  .connectable = 1 };
    if (ble->adv_start(&s_adv)) {
        snprintf(s_status, sizeof(s_status), "Could not start advertising");
        return false;
    }
    s_ok = true;
    papp_ble_addr_t me;
    if (!ble->own_addr(&me)) ui_log(2, "Bluetooth address %s, advertising as \"%s\"", addr_str(&me), NAME);
    update_status();
    return true;
}

static void ble_events(void)
{
    papp_ble_event_t e;
    while (s_ok && ble->poll_event(&e)) {
        int c = e.conn;
        switch (e.type) {
        case PAPP_BLE_EV_CONNECTED:
            if (e.code || c >= PAPP_BLE_MAX_CONN) break;
            s_live[c] = true;
            s_mtu[c] = 23;
            s_rxn[c] = 0;
            s_peer[c] = e.addr;
            s_conns++;
            ui_log(2, "Connected: %s", addr_str(&e.addr));
            if (s_conns < PAPP_BLE_MAX_CONN) ble->adv_start(&s_adv);     /* room for one more */
            break;
        case PAPP_BLE_EV_DISCONNECTED:
            if (c < PAPP_BLE_MAX_CONN && s_live[c]) {
                s_live[c] = false;
                s_conns--;
            }
            ui_log(2, "Disconnected (reason 0x%x)", e.code);
            ble->adv_start(&s_adv);
            break;
        case PAPP_BLE_EV_MTU:
            if (c < PAPP_BLE_MAX_CONN) s_mtu[c] = e.code;
            break;
        case PAPP_BLE_EV_WRITE:
            if (e.handle == h_rx) {
                console_bytes((papp_ble_conn_t)c, e.data, e.len);
            } else if (e.handle == h_mode && e.len == 1 && e.data[0] < 3) {
                remote.mode = e.data[0];
                changed(REMOTE_MODE, "Bluetooth");
            } else if (e.handle == h_level && e.len == 1 && e.data[0] <= 100) {
                remote.level = e.data[0];
                changed(REMOTE_LEVEL, "Bluetooth");
            } else if (e.handle == h_led && e.len == 1) {
                remote.led = e.data[0] != 0;
                changed(REMOTE_LED, "Bluetooth");
            } else if (e.handle == h_msg) {
                int n = e.len < REMOTE_MSG_MAX ? e.len : REMOTE_MSG_MAX;
                memcpy(remote.msg, e.data, n);
                remote.msg[n] = 0;
                changed(REMOTE_MSG, "Bluetooth");
            }
            break;
        default:
            break;
        }
        update_status();
    }
}

/* ── Screen ────────────────────────────────────────────────────────────── */

static void send_typed(void)
{
    if (!s_typen || !s_conns) return;
    s_type[s_typen] = 0;
    reply(PAPP_BLE_CONN_ALL, "%s\r\n", s_type);
    s_typen = 0;
}

static void act(int id, int x)
{
    if (id >= UI_ID_TAB && id < UI_ID_TAB + 2) s_tab = id - UI_ID_TAB;
    else if (id == UI_ID_CLOSE) s_quit = true;
    else if (id == H_SEND) send_typed();
    else if (id == H_HELLO && s_conns) send_text(PAPP_BLE_CONN_ALL, "Hello from the P4!\r\n");
    else if (id == H_CLEAR) ui_log_clear();
    else publish(remote_act(id, x));
}

static void draw_ble(void)
{
    ui_log_draw(20, 112, 760, 288, "Nothing received yet");
    ui_rrect(20, 412, 400, 52, 26, C_RAISED);
    if (s_typen) {
        int x = ui_text_n(ui_med, 40, ui_line_top(ui_med, 438), s_type, s_typen, C_TEXT, 400);
        if ((papp_time_us() / 500000) & 1) ui_fill(x + 2, 424, 2, 28, C_TEXT);
    } else {
        ui_text_l(ui_small, 40, 438, "Type on a USB keyboard, Enter sends", C_FAINT);
    }
    ui_button(432, 412, 108, 52, "Send", s_typen && s_conns ? C_BLUE : C_BORDER, H_SEND);
    ui_button(552, 412, 108, 52, "Hello", s_conns ? C_BLUE : C_BORDER, H_HELLO);
    ui_button(672, 412, 108, 52, "Clear", C_RAISED, H_CLEAR);
}

static void keyboard(void)
{
    papp_input_event_t ev;
    while (papp_input_poll_event(&ev)) {
        if (ev.type != PAPP_EV_KEY_DOWN) continue;
        if (ev.code == 0x28 || ev.code == 0x58) send_typed();               /* Enter */
        else if (ev.code == 0x2A) { if (s_typen) s_typen--; }               /* Backspace */
        else if (ev.code == 0x29) s_typen = 0;                              /* Esc */
        else if (ev.text >= 32 && ev.text < 127 && s_typen < (int)sizeof(s_type) - 1) {
            s_type[s_typen++] = (char)ev.text;
            s_tab = TAB_BLE;
        }
    }
}

static void draw(papp_surface_t *s)
{
    static const char *const tabs[2] = { "Control", "BLE" };
    ui_begin(s);
    ui_bar("BLE Remote", tabs, 2, s_tab, s_status, !s_ok ? C_RED : s_conns ? C_GREEN : C_ORANGE,
           s_ok && !s_conns);
    if (s_tab == TAB_CONTROL) remote_draw("Change here, from a phone or a PC");
    else draw_ble();
    ui_end(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    if (!s) return 1;
    draw(s);                                  /* one frame first: starting Bluetooth takes a moment */
    ble_start();

    uint32_t prev = 0;
    while (!papp_exit_requested() && !s_quit) {
        ble_events();
        keyboard();

        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_L1) s_tab = TAB_CONTROL;
        if (press & PAPP_BTN_R1) s_tab = TAB_BLE;
        publish(remote_pad(press));
        if (press & (PAPP_BTN_START | PAPP_BTN_X)) {
            static const char *const items[] = { "Control", "BLE", "Exit" };
            int r = papp_ui_menu("BLE Remote", items, 3, s_tab);
            if (r == 2) break;
            if (r >= 0) s_tab = r;
            prev = 0xFFFFFFFFu;
            ui_touch_reset();
            continue;
        }
        ui_touch(s, act);
        draw(s);
    }
    papp_disp_surface_destroy(s);
    return 0;
}

/*
 * Wi-Fi Remote — the board as a small web server: a phone or PC browser on the same network
 * opens http://<board's address>/ and sets the mode, level, LED and message, like the Gution
 * ESP32-P4 firmware's web page. The same values as BLE Remote (common/edu_ui.c).
 *
 * HTTP on port 80:
 *   GET  /             the control page (polls /state every second)
 *   GET  /state        {"mode":1,"level":50,"led":true,"label":"Hi","rev":7}   (the Gution format)
 *   POST /state        a JSON object with any of mode, level, led, label: applies it, replies /state
 *   GET  /set?level=80&led=on&mode=2&msg=Hi    the same from a browser's address bar or curl
 * Any origin may call it (CORS), so a student's own web page or script can drive the board.
 *
 * Network: the saved Wi-Fi network (Settings on the home screen). The Network button (or
 * "Choose network" in the menu) opens the launcher's Wi-Fi picker: the network chosen there
 * becomes the board's saved network. Without one, or with the Hotspot button, the board also
 * opens its own network "P4-Remote-xxxx" (password p4remote), where the page is at
 * http://192.168.4.1/.
 *
 * Screen: Control (as BLE Remote) and Wi-Fi (the address to open, hotspot, a log of requests).
 * Pad: L1 / R1 tab, UP / DOWN mode, LEFT / RIGHT level, A LED, Y hotspot, START menu.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "papp/papp.h"
#include "edu_ui.h"

/* An education app: built into SDcard/apps/Education (apps/education/build.py), never into the Store */
PAPP_MANIFEST(.id = "edu.wifiremote", .name = "Wi-Fi Remote", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define PORT      80
#define AP_PASS   "p4remote"
#define MAX_CLI   6                           /* + the listener: within PAPP_NET_MAX_SOCKETS */
#define REQ_MAX   2048

enum { TAB_CONTROL, TAB_WIFI };
enum { H_HOTSPOT = 1, H_CLEAR, H_NETWORK };

static const papp_if_net_t *net;
static papp_sock_t *s_listen;
static papp_net_info_t s_info;
static char s_ap_ssid[33];
static char s_status[96] = "Starting Wi-Fi...";
static int  s_tab = TAB_WIFI, s_requests, s_polls;
static bool s_serving, s_quit, s_after_ui;
static int64_t s_info_next;

typedef struct {
    papp_sock_t *s;
    papp_addr_t from;
    int64_t since;
    int n;
    char b[REQ_MAX + 1];
} client_t;
static client_t s_cli[MAX_CLI];

static const char k_page[] =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>P4 Wi-Fi Remote</title><style>"
"body{margin:0;font-family:system-ui,sans-serif;background:#081226;color:#fff}"
"main{max-width:460px;margin:auto;padding:16px}h1{font-size:22px;margin:8px 0 16px}"
".card{background:#0e1a30;border-radius:16px;padding:16px;margin-bottom:14px}"
".k{color:#8fa3bf;font-size:12px;letter-spacing:.08em;margin-bottom:10px}"
".seg{display:flex;gap:8px}.seg button{flex:1}"
"button{background:#16274a;color:#8fa3bf;border:0;border-radius:22px;padding:12px;font-size:16px}"
"button.on{background:#1e6fff;color:#fff}input[type=range]{width:100%}"
".row{display:flex;align-items:center;justify-content:space-between}"
".sw{width:64px;height:34px;border-radius:17px;background:#24406e;position:relative;cursor:pointer}"
".sw::after{content:'';position:absolute;top:3px;left:3px;width:28px;height:28px;border-radius:50%;"
"background:#fff;transition:.15s}.sw.on{background:#3ddc84}.sw.on::after{left:33px}"
".msg{display:flex;gap:8px}.msg input{flex:1;min-width:0;background:#16274a;border:0;border-radius:12px;"
"color:#fff;padding:12px;font-size:16px}#st{color:#4a5c7a;font-size:13px;text-align:center}"
"</style></head><body><main><h1>ESP32-P4 Wi-Fi Remote</h1>"
"<div class=card><div class=k>MODE</div><div class=seg id=mode><button>Idle</button><button>Run</button>"
"<button>Test</button></div></div>"
"<div class=card><div class='row k'><span>LEVEL</span><span id=lv></span></div>"
"<input type=range id=level min=0 max=100></div>"
"<div class='card row'><span>LED</span><div class=sw id=led></div></div>"
"<div class=card><div class=k>MESSAGE</div><div class=msg><input id=msg maxlength=32>"
"<button class=on id=send>Send</button></div></div><div id=st>Connecting...</div></main><script>"
"let rev=-1,busy=0,t=0;const $=i=>document.getElementById(i);"
"const ok=()=>$('st').textContent='Connected, '+new Date().toLocaleTimeString();"
"function show(s){rev=s.rev;[...$('mode').children].forEach((b,i)=>b.className=i==s.mode?'on':'');"
"if(!busy)$('level').value=s.level;$('lv').textContent=busy?$('level').value:s.level;"
"$('led').className='sw'+(s.led?' on':'');if(document.activeElement!=$('msg'))$('msg').value=s.label;ok()}"
"function post(o){fetch('/state',{method:'POST',body:JSON.stringify(o)}).then(r=>r.json()).then(show)"
".catch(e=>$('st').textContent='Offline')}"
"function poll(){fetch('/state').then(r=>r.json()).then(s=>{if(s.rev!=rev)show(s);else ok()})"
".catch(e=>$('st').textContent='Offline').finally(()=>setTimeout(poll,1000))}"
"[...$('mode').children].forEach((b,i)=>b.onclick=()=>post({mode:i}));"
"$('level').oninput=e=>{busy=1;$('lv').textContent=e.target.value;"
"if(Date.now()-t>150){t=Date.now();post({level:+e.target.value})}};"
"$('level').onchange=e=>{busy=0;post({level:+e.target.value})};"
"$('led').onclick=()=>post({led:!$('led').classList.contains('on')});"
"$('send').onclick=()=>post({label:$('msg').value});"
"$('msg').onkeydown=e=>{if(e.key=='Enter')$('send').click()};poll();"
"</script></body></html>";

static const char *ip_str(const uint8_t *ip)
{
    static char b[2][16];
    static int k;
    k ^= 1;
    snprintf(b[k], sizeof(b[k]), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return b[k];
}

/* ── Applying changes ─────────────────────────────────────────────────── */

static int set_mode(int v) { if (v < 0 || v > 2 || v == remote.mode) return 0; remote.mode = (uint8_t)v; return REMOTE_MODE; }
static int set_level(int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v == remote.level) return 0;
    remote.level = (uint8_t)v;
    return REMOTE_LEVEL;
}
static int set_led(bool on) { if (on == remote.led) return 0; remote.led = on; return REMOTE_LED; }
static int set_msg(const char *s)
{
    if (!strncmp(s, remote.msg, REMOTE_MSG_MAX) && strlen(s) <= REMOTE_MSG_MAX) return 0;
    snprintf(remote.msg, sizeof(remote.msg), "%s", s);
    return REMOTE_MSG;
}

/* The flat JSON the page sends: "key": value — no parser needed (as the Gution firmware) */
static const char *json_value(const char *body, const char *key)
{
    char pat[16];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return NULL;
    p++;
    while (*p == ' ') p++;
    return p;
}

static int apply_json(const char *body)
{
    int what = 0;
    const char *v;
    if ((v = json_value(body, "mode"))) what |= set_mode(atoi(v));
    if ((v = json_value(body, "level"))) what |= set_level(atoi(v));
    if ((v = json_value(body, "led"))) what |= set_led(!strncmp(v, "true", 4) || *v == '1');
    if ((v = json_value(body, "label")) || (v = json_value(body, "msg"))) {
        if (*v == '"') {
            char s[REMOTE_MSG_MAX + 1];
            int o = 0;
            for (v++; *v && *v != '"' && o < REMOTE_MSG_MAX; v++) {
                if (*v == '\\' && v[1]) v++;
                s[o++] = *v;
            }
            s[o] = 0;
            what |= set_msg(s);
        }
    }
    return what;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10) & 15; }

/* /set?level=80&led=on: query values are URL-encoded */
static int apply_query(char *q)
{
    int what = 0;
    for (char *p = q, *next; p && *p; p = next) {
        next = strchr(p, '&');
        if (next) *next++ = 0;
        char *v = strchr(p, '=');
        if (!v) continue;
        *v++ = 0;
        char *o = v;
        for (char *i = v; *i; i++) {
            if (*i == '+') *o++ = ' ';
            else if (*i == '%' && isxdigit((unsigned char)i[1]) && isxdigit((unsigned char)i[2])) {
                *o++ = (char)(hexval(i[1]) * 16 + hexval(i[2]));
                i += 2;
            } else *o++ = *i;
        }
        *o = 0;
        if (!strcmp(p, "mode")) what |= set_mode(atoi(v));
        else if (!strcmp(p, "level")) what |= set_level(atoi(v));
        else if (!strcmp(p, "led")) what |= set_led(!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true"));
        else if (!strcmp(p, "msg") || !strcmp(p, "label")) what |= set_msg(v);
    }
    return what;
}

static int state_json(char *out, int len)
{
    char safe[REMOTE_MSG_MAX * 2 + 1];
    int o = 0;
    for (const char *p = remote.msg; *p && o + 2 < (int)sizeof(safe); p++) {
        if ((unsigned char)*p < 0x20) continue;
        if (*p == '"' || *p == '\\') safe[o++] = '\\';
        safe[o++] = *p;
    }
    safe[o] = 0;
    return snprintf(out, len, "{\"mode\":%d,\"level\":%d,\"led\":%s,\"label\":\"%s\",\"rev\":%lu}", remote.mode,
                    remote.level, remote.led ? "true" : "false", safe, (unsigned long)remote.rev);
}

/* ── HTTP ──────────────────────────────────────────────────────────────── */

static void respond(client_t *c, int code, const char *type, const char *body, int len)
{
    const char *why = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request"
                    : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : "Error";
    char h[320];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n",
                     code, why, type, len);
    net->send(c->s, h, n, 3000);
    for (int sent = 0; sent < len;) {
        int r = net->send(c->s, body + sent, len - sent, 3000);
        if (r <= 0) break;
        sent += r;
    }
}

/* Before the reply is made, so it carries the new rev */
static void changed(int what, const char *who)
{
    char src[40];
    snprintf(src, sizeof(src), "browser %s", who);
    remote_changed(what, src);
}

static void handle(client_t *c, char *body)
{
    char *sp = strchr(c->b, ' '), *path = sp ? sp + 1 : NULL, *end = path ? strchr(path, ' ') : NULL;
    if (!end) {
        respond(c, 400, "text/plain", "bad request\n", 12);
        return;
    }
    *sp = 0;
    *end = 0;
    const char *method = c->b, *who = ip_str(c->from.ip);
    char *query = strchr(path, '?');
    if (query) *query++ = 0;
    s_requests++;

    char json[160];
    if (!strcmp(method, "OPTIONS")) {
        respond(c, 204, "text/plain", "", 0);
    } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(c, 200, "text/html; charset=utf-8", k_page, (int)sizeof(k_page) - 1);
        ui_log(0, "%s: opened the page", who);
    } else if (!strcmp(path, "/state") && !strcmp(method, "GET")) {
        s_polls++;
        respond(c, 200, "application/json", json, state_json(json, sizeof(json)));
    } else if (!strcmp(path, "/state") && !strcmp(method, "POST")) {
        ui_log_bytes(0, who, (int)strlen(who));
        ui_log_bytes(0, ": POST ", 7);
        ui_log_bytes(0, body, (int)strlen(body));
        ui_log_bytes(0, "\n", 1);
        changed(apply_json(body), who);
        int n = state_json(json, sizeof(json));
        respond(c, 200, "application/json", json, n);
        ui_log(1, "%s", json);
    } else if (!strcmp(path, "/set") && !strcmp(method, "GET")) {
        ui_log(0, "%s: GET /set?%s", who, query ? query : "");
        if (query) changed(apply_query(query), who);
        int n = state_json(json, sizeof(json));
        respond(c, 200, "application/json", json, n);
        ui_log(1, "%s", json);
    } else if (!strcmp(path, "/state") || !strcmp(path, "/set")) {
        respond(c, 405, "text/plain", "method not allowed\n", 19);
    } else {
        respond(c, 404, "text/plain", "not found\n", 10);
        if (strcmp(path, "/favicon.ico")) ui_log(0, "%s: %s %s -> 404", who, method, path);
    }
}

static void client_close(client_t *c)
{
    net->close(c->s);
    c->s = NULL;
}

/* Whole request in? Then answer it and close (one request per connection) */
static void client_check(client_t *c)
{
    c->b[c->n] = 0;
    char *hend = strstr(c->b, "\r\n\r\n");
    if (!hend) {
        if (c->n >= REQ_MAX) {
            respond(c, 400, "text/plain", "request too large\n", 18);
            client_close(c);
        }
        return;
    }
    int clen = 0;
    for (char *p = c->b; p < hend; p = strstr(p, "\r\n") + 2) {
        if (!strncasecmp(p, "Content-Length:", 15)) clen = atoi(p + 15);
        if (!strstr(p, "\r\n")) break;
    }
    char *body = hend + 4;
    if (body + clen > c->b + REQ_MAX) {
        respond(c, 400, "text/plain", "request too large\n", 18);
        client_close(c);
        return;
    }
    if (body + clen > c->b + c->n) return;            /* the body is still coming */
    body[clen] = 0;
    *hend = 0;
    handle(c, body);
    client_close(c);
}

static void serve(void)
{
    if (!s_serving) return;
    papp_poll_t set[1 + MAX_CLI];
    client_t *who[1 + MAX_CLI];
    int n = 0, free_slot = -1;
    for (int i = 0; i < MAX_CLI; i++) {
        if (s_cli[i].s) {
            set[n] = (papp_poll_t){ s_cli[i].s, PAPP_POLL_IN, 0 };
            who[n++] = &s_cli[i];
        } else if (free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        set[n] = (papp_poll_t){ s_listen, PAPP_POLL_IN, 0 };
        who[n++] = NULL;
    }
    if (net->poll(set, n, 0) <= 0) goto timeouts;
    for (int i = 0; i < n; i++) {
        if (!set[i].got) continue;
        if (!who[i]) {
            client_t *c = &s_cli[free_slot];
            c->s = net->accept(s_listen, &c->from, 0, NULL);
            c->n = 0;
            c->since = papp_time_us();
            continue;
        }
        client_t *c = who[i];
        int r = net->recv(c->s, c->b + c->n, REQ_MAX - c->n, 0);
        if (r <= 0 && r != PAPP_E_TIMEOUT) client_close(c);
        else if (r > 0) {
            c->n += r;
            client_check(c);
        }
    }
timeouts:
    for (int i = 0; i < MAX_CLI; i++)            /* a browser's spare connection that never asks */
        if (s_cli[i].s && papp_time_us() - s_cli[i].since > 5000000) client_close(&s_cli[i]);
}

/* ── Network ───────────────────────────────────────────────────────────── */

static void update_status(void)
{
    net->info(&s_info);
    bool sta = s_info.state == PAPP_NET_CONNECTED;
    if (!s_serving) return;
    if (sta && s_info.ap_running)
        snprintf(s_status, sizeof(s_status), "Serving http://%s and http://%s (hotspot)", ip_str(s_info.ip),
                 ip_str(s_info.ap_ip));
    else if (sta)
        snprintf(s_status, sizeof(s_status), "Serving http://%s on \"%s\"", ip_str(s_info.ip), s_info.ssid);
    else if (s_info.ap_running)
        snprintf(s_status, sizeof(s_status), "Serving http://%s on the hotspot \"%s\"", ip_str(s_info.ap_ip),
                 s_ap_ssid);
    else
        snprintf(s_status, sizeof(s_status), "Not connected to Wi-Fi - turn on the hotspot");
}

static void hotspot(bool on)
{
    if (!on) {
        net->ap_stop();
        ui_log(2, "Hotspot off");
    } else {
        int r = net->ap_start(s_ap_ssid, AP_PASS, 0, 4);
        if (r == PAPP_E_OK) ui_log(2, "Hotspot \"%s\" on, password %s", s_ap_ssid, AP_PASS);
        else ui_log(2, "Hotspot failed (%d)", r);
    }
    update_status();
}

static void net_start(void)
{
    net = papp_net();
    if (!net) {
        snprintf(s_status, sizeof(s_status), "No Wi-Fi on this board or launcher");
        return;
    }
    snprintf(s_status, sizeof(s_status), "Connecting to Wi-Fi...");
    int r = net->up(15000);
    papp_net_event_t e;
    while (net->poll_event(&e)) {}            /* what happened while joining: the launcher's own pause */
    net->info(&s_info);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "P4-Remote-%02X%02X", s_info.mac[4], s_info.mac[5]);
    if (r == PAPP_E_OK) {
        ui_log(2, "Wi-Fi \"%s\", address %s", s_info.ssid, ip_str(s_info.ip));
    } else {
        ui_log(2, "No Wi-Fi network (%d): starting the hotspot. Network chooses one.", r);
        hotspot(true);
    }

    int err = 0;
    s_listen = net->socket(PAPP_SOCK_TCP, &err);
    if (s_listen) {
        net->set_opt(s_listen, PAPP_SO_REUSEADDR, 1);
        if (net->bind(s_listen, PORT) == PAPP_E_OK && net->listen(s_listen, 4) == PAPP_E_OK) s_serving = true;
    }
    if (!s_serving) {
        snprintf(s_status, sizeof(s_status), "Could not start the web server (%d)", err);
        return;
    }
    update_status();
}

static void net_events(void)
{
    if (!net) return;
    papp_net_event_t e;
    bool any = false;
    while (net->poll_event(&e)) {
        any = true;
        if (e.type == PAPP_NET_EV_CONNECTED) ui_log(2, "Wi-Fi connected");
        else if (e.type == PAPP_NET_EV_DISCONNECTED) ui_log(2, "Wi-Fi disconnected (reason %u)", e.code);
        else if (e.type == PAPP_NET_EV_AP_CLIENT_JOINED) ui_log(2, "A device joined the hotspot (%u now)", e.code);
        else if (e.type == PAPP_NET_EV_AP_CLIENT_LEFT) ui_log(2, "A device left the hotspot (%u now)", e.code);
    }
    int64_t now = papp_time_us();
    if (any || now >= s_info_next) {
        s_info_next = now + 1000000;
        update_status();
    }
}

/* The launcher's picker: the network chosen is saved for the board, not only for this run */
static void choose_network(void)
{
    int r = papp_ui_wifi();
    s_after_ui = true;
    papp_net_event_t e;
    while (net->poll_event(&e)) {}            /* the drops and joins while the user chose */
    if (r == 1) {
        net->info(&s_info);
        ui_log(2, "Wi-Fi \"%s\" chosen and saved, address %s", s_info.ssid, ip_str(s_info.ip));
    } else if (r < 0 && r != PAPP_E_EXIT) {
        ui_log(2, "No Wi-Fi picker on this launcher (%d)", r);
    }
    update_status();
}

/* ── Screen ────────────────────────────────────────────────────────────── */

static void act(int id, int x)
{
    if (id >= UI_ID_TAB && id < UI_ID_TAB + 2) s_tab = id - UI_ID_TAB;
    else if (id == UI_ID_CLOSE) s_quit = true;
    else if (id == H_HOTSPOT && s_serving) hotspot(!s_info.ap_running);
    else if (id == H_NETWORK && net) choose_network();
    else if (id == H_CLEAR) ui_log_clear();
    else remote_act(id, x);
}

static void draw_wifi(void)
{
    char b[96];
    bool sta = s_info.state == PAPP_NET_CONNECTED;
    ui_rrect(20, 112, 760, 166, 18, C_PANEL);
    ui_text_l(ui_small, 44, 138, "OPEN IN A BROWSER", C_FAINT);
    int y = 176;
    if (sta) {
        snprintf(b, sizeof(b), "http://%s", ip_str(s_info.ip));
        int x = ui_text_n(ui_med, 44, ui_line_top(ui_med, y), b, 99, C_TEXT, 600);
        snprintf(b, sizeof(b), "  on \"%s\"", s_info.ssid);
        ui_text_n(ui_small, x, ui_line_top(ui_small, y), b, 99, C_DIM, 580);
        y += 44;
    }
    if (s_info.ap_running) {
        snprintf(b, sizeof(b), "http://%s", ip_str(s_info.ap_ip));
        int x = ui_text_n(ui_med, 44, ui_line_top(ui_med, y), b, 99, C_TEXT, 600);
        ui_text_n(ui_small, x, ui_line_top(ui_small, y), "  on the hotspot", 99, C_DIM, 590);
        snprintf(b, sizeof(b), "Join Wi-Fi \"%s\", password %s", s_ap_ssid, AP_PASS);
        ui_text_l(ui_small, 44, y + 30, b, C_ORANGE);
        y += 74;
    }
    if (!sta && !s_info.ap_running) {
        ui_text_l(ui_small, 44, y, s_serving ? "No network: tap Network or Hotspot" : s_status, C_DIM);
        y += 44;
    }
    if (y < 250) {
        snprintf(b, sizeof(b), "%d requests, %d of them /state polls", s_requests, s_polls);
        ui_text_l(ui_small, 44, 250, b, C_FAINT);
    }
    ui_button(604, 126, 156, 44, "Network", C_RAISED, H_NETWORK);
    ui_button(604, 178, 156, 44, "Hotspot", s_info.ap_running ? C_GREEN : C_RAISED, H_HOTSPOT);
    ui_button(604, 230, 156, 40, "Clear log", C_RAISED, H_CLEAR);
    ui_log_draw(20, 290, 760, 174, "Requests from browsers appear here");
}

static void draw(papp_surface_t *s)
{
    static const char *const tabs[2] = { "Control", "Wi-Fi" };
    bool up = s_serving && (s_info.state == PAPP_NET_CONNECTED || s_info.ap_running);
    ui_begin(s);
    ui_bar("Wi-Fi Remote", tabs, 2, s_tab, s_status, !net || (s_serving && !up) ? C_RED : up ? C_GREEN : C_ORANGE,
           !up);
    if (s_tab == TAB_CONTROL) remote_draw("Change here or from a browser");
    else draw_wifi();
    ui_end(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    if (!s) return 1;
    draw(s);                                  /* one frame first: joining the network takes a moment */
    net_start();

    uint32_t prev = 0;
    while (!papp_exit_requested() && !s_quit) {
        serve();
        net_events();

        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_L1) s_tab = TAB_CONTROL;
        if (press & PAPP_BTN_R1) s_tab = TAB_WIFI;
        remote_pad(press);
        if ((press & PAPP_BTN_Y) && s_serving) hotspot(!s_info.ap_running);
        if (press & (PAPP_BTN_START | PAPP_BTN_X)) {
            static const char *const items[] = { "Control", "Wi-Fi", "Choose network", "Exit" };
            int r = papp_ui_menu("Wi-Fi Remote", items, net ? 4 : 3, s_tab);
            if (r == (net ? 3 : 2)) break;
            if (r == 2 && net) {
                s_tab = TAB_WIFI;
                choose_network();
            } else if (r >= 0) {
                s_tab = r;
            }
            prev = 0xFFFFFFFFu;
            ui_touch_reset();
            continue;
        }
        ui_touch(s, act);
        if (s_after_ui) {                     /* the picker took the input: start clean */
            s_after_ui = false;
            prev = 0xFFFFFFFFu;
            ui_touch_reset();
        }
        draw(s);
    }
    for (int i = 0; i < MAX_CLI; i++)
        if (s_cli[i].s) client_close(&s_cli[i]);
    if (s_listen) net->close(s_listen);
    papp_disp_surface_destroy(s);
    return 0;
}

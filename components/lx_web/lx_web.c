/*
 * Wi-Fi file manager: a small web page for the SD card, so ROMs can be uploaded
 * from a phone or PC. Off until started from the Wi-Fi screen, and every request
 * except the page itself needs the PIN shown on that screen.
 */
#include "lx_web.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lx_common.h"

static const char *TAG = "lx_web";

#define CHUNK    8192
#define PATH_MAX_LEN 200

static httpd_handle_t s_server;
static char s_pin[8];
static volatile uint32_t s_changes;

static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>P4 Launcher files</title><style>"
"body{font:16px system-ui;margin:0;background:#0b1630;color:#fff}main{max-width:760px;margin:auto;padding:16px}"
"a{color:#3bc9f5;text-decoration:none}li{display:flex;gap:12px;padding:10px 0;border-bottom:1px solid #24406e}"
"li a:first-child{flex:1;word-break:break-all}ul{list-style:none;padding:0}button,input{font:inherit;padding:6px 10px}"
"small{color:#8fa3bf}</style></head><body><main><h1>P4 Launcher files</h1><p id=path></p>"
"<p><input type=file id=f multiple> <button onclick=up()>Upload</button> <button onclick=md()>New folder</button></p>"
"<p id=st></p><ul id=l></ul></main><script>"
"let pin=prompt('PIN shown on the Wi-Fi screen')||'',cwd='/sd';"
"const q=(a,p)=>'/api/'+a+'?pin='+pin+'&path='+encodeURIComponent(p);"
"const st=t=>document.getElementById('st').textContent=t;"
"async function ls(p){const r=await fetch(q('ls',p));if(!r.ok){st('Error: '+await r.text());return}"
"cwd=p;document.getElementById('path').textContent=p;const u=document.getElementById('l');u.textContent='';"
"if(p!='/sd')add('..',1,0,p.slice(0,p.lastIndexOf('/')));"
"for(const e of await r.json())add(e.n,e.d,e.s,p+'/'+e.n)}"
"function add(n,d,s,p){const li=document.createElement('li'),a=document.createElement('a');a.textContent=d?n+'/':n;"
"a.href=d?'#':q('get',p);if(d)a.onclick=()=>{ls(p);return false};else a.download=n;li.append(a);"
"if(n!='..'){if(!d){const m=document.createElement('small');m.textContent=(s/1048576).toFixed(2)+' MB';li.append(m)}"
"const b=document.createElement('button');b.textContent='Delete';b.onclick=()=>rm(p,n);li.append(b)}"
"document.getElementById('l').append(li)}"
"async function rm(p,n){if(!confirm('Delete '+n+'?'))return;const r=await fetch(q('rm',p),{method:'POST'});"
"st(r.ok?'Deleted':'Error: '+await r.text());ls(cwd)}"
"async function md(){const n=prompt('Folder name');if(!n)return;const r=await fetch(q('mkdir',cwd+'/'+n),{method:'POST'});"
"st(r.ok?'Created':'Error: '+await r.text());ls(cwd)}"
"async function up(){for(const f of document.getElementById('f').files){st('Uploading '+f.name+'...');"
"const r=await fetch(q('put',cwd+'/'+f.name),{method:'PUT',body:f});if(!r.ok){st('Error: '+await r.text());return}}"
"st('Done');ls(cwd)}ls(cwd)</script></body></html>";

/* Logged so a page that won't open shows whether its traffic reaches the board */
static esp_err_t on_open(httpd_handle_t h, int fd)
{
    struct sockaddr_in6 peer;
    socklen_t len = sizeof(peer);
    char ip[48] = "?";
    if (getpeername(fd, (struct sockaddr *)&peer, &len) == 0)
        inet_ntop(AF_INET, &peer.sin6_addr.un.u32_addr[3], ip, sizeof(ip));
    ESP_LOGI(TAG, "connection from %s", ip);
    return ESP_OK;
}

static esp_err_t page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s == '+' ? ' ' : *s;
        }
    }
    *o = 0;
}

/* The PIN and a path inside /sd; on failure the reply is already sent */
static bool get_path(httpd_req_t *r, char *path)
{
    char q[PATH_MAX_LEN * 3 + 32], pin[8] = "";
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "pin", pin, sizeof(pin)) != ESP_OK || strcmp(pin, s_pin) != 0) {
        httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Wrong PIN");
        return false;
    }
    if (httpd_query_key_value(q, "path", path, PATH_MAX_LEN) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "No path");
        return false;
    }
    url_decode(path);
    if (strncmp(path, "/sd", 3) != 0 || (path[3] && path[3] != '/') || strstr(path, "..")) {
        httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Outside the SD card");
        return false;
    }
    return true;
}

static esp_err_t sd_busy(httpd_req_t *r)
{
    httpd_resp_set_status(r, "503 Service Unavailable");
    return httpd_resp_sendstr(r, "Another SD write is in progress");
}

static esp_err_t api_ls(httpd_req_t *r)
{
    char path[PATH_MAX_LEN], full[PATH_MAX_LEN + 260], esc[520];
    if (!get_path(r, path)) return ESP_OK;
    DIR *d = opendir(path);
    if (!d) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "No such folder");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr_chunk(r, "[");
    struct dirent *e;
    bool first = true;
    while ((e = readdir(d))) {
        struct stat st = { 0 };
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        stat(full, &st);
        char *o = esc;
        for (const char *s = e->d_name; *s && o < esc + sizeof(esc) - 3; s++) {
            if (*s == '"' || *s == '\\') *o++ = '\\';
            *o++ = *s;
        }
        *o = 0;
        char row[600];
        snprintf(row, sizeof(row), "%s{\"n\":\"%s\",\"d\":%d,\"s\":%ld}", first ? "" : ",", esc,
                 e->d_type == DT_DIR, (long)st.st_size);
        httpd_resp_sendstr_chunk(r, row);
        first = false;
    }
    closedir(d);
    httpd_resp_sendstr_chunk(r, "]");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t api_get(httpd_req_t *r)
{
    char path[PATH_MAX_LEN];
    if (!get_path(r, path)) return ESP_OK;
    FILE *f = fopen(path, "rb");
    char *buf = malloc(CHUNK);
    if (!f || !buf) {
        if (f) fclose(f);
        free(buf);
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Cannot open the file");
    }
    httpd_resp_set_type(r, "application/octet-stream");
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) err = httpd_resp_send_chunk(r, buf, n);
    fclose(f);
    free(buf);
    if (err == ESP_OK) httpd_resp_send_chunk(r, NULL, 0);
    return err;
}

/* Uploads use unique staging names and a shared lock so they cannot collide
 * with DevLink or another mutation while the file is being committed. */
static esp_err_t api_put(httpd_req_t *r)
{
    char path[PATH_MAX_LEN], tmp[PATH_MAX_LEN + 32];
    if (!get_path(r, path)) return ESP_OK;
    if (!lx_sd_write_try_lock()) return sd_busy(r);
    int tmp_len = snprintf(tmp, sizeof(tmp), "%s.web-%08lx.part", path,
                           (unsigned long)esp_random());
    if (tmp_len < 0 || (size_t)tmp_len >= sizeof(tmp)) {
        lx_sd_write_unlock();
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "File path is too long");
    }
    FILE *f = fopen(tmp, "wb");
    char *buf = malloc(CHUNK);
    bool ok = f && buf;
    size_t left = r->content_len;
    while (ok && left) {
        int n = httpd_req_recv(r, buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        ok = n > 0 && fwrite(buf, 1, n, f) == (size_t)n;
        if (ok) left -= n;
    }
    if (f && fclose(f) != 0) ok = false;
    free(buf);
    if (ok) {
        ok = lx_sd_replace_file(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        lx_sd_write_unlock();
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }
    ESP_LOGI(TAG, "received %s", path);
    s_changes++;
    lx_sd_write_unlock();
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t api_mkdir(httpd_req_t *r)
{
    char path[PATH_MAX_LEN];
    if (!get_path(r, path)) return ESP_OK;
    if (!lx_sd_write_try_lock()) return sd_busy(r);
    int result = mkdir(path, 0775);
    lx_sd_write_unlock();
    if (result != 0) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create it");
    s_changes++;
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t api_rm(httpd_req_t *r)
{
    char path[PATH_MAX_LEN];
    if (!get_path(r, path)) return ESP_OK;
    if (!lx_sd_write_try_lock()) return sd_busy(r);
    int result = strcmp(path, "/sd") == 0 ? -1 : remove(path);
    if (result != 0 && strcmp(path, "/sd") != 0) result = rmdir(path);
    lx_sd_write_unlock();
    if (result != 0)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot delete it (a folder must be empty)");
    ESP_LOGI(TAG, "deleted %s", path);
    s_changes++;
    return httpd_resp_sendstr(r, "ok");
}

bool lx_web_running(void)
{
    return s_server != NULL;
}

uint32_t lx_web_changes(void)
{
    return s_changes;
}

const char *lx_web_pin(void)
{
    return s_pin;
}

esp_err_t lx_web_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.open_fn = on_open;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }
    snprintf(s_pin, sizeof(s_pin), "%04u", (unsigned)(esp_random() % 10000));
    static const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page },
        { .uri = "/api/ls", .method = HTTP_GET, .handler = api_ls },
        { .uri = "/api/get", .method = HTTP_GET, .handler = api_get },
        { .uri = "/api/put", .method = HTTP_PUT, .handler = api_put },
        { .uri = "/api/mkdir", .method = HTTP_POST, .handler = api_mkdir },
        { .uri = "/api/rm", .method = HTTP_POST, .handler = api_rm },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) httpd_register_uri_handler(s_server, &routes[i]);
    ESP_LOGI(TAG, "file manager on, PIN set");
    return ESP_OK;
}

void lx_web_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    s_pin[0] = 0;
}

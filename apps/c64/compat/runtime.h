#pragma once
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif
extern const app_services_t *_pax_svc;
extern void pax_cleanup_heap(void);
extern void pax_cleanup_fds(void);
extern void pax_close_streams(void);
int pax_jit_sync(void *address, size_t size);
#ifdef __cplusplus
}
#endif

static int emu_extension_allowed(const char *path, const char *extensions)
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    char token[24];
    if (!ext || !extensions || strlen(ext) > 16) return 0;
    snprintf(token, sizeof(token), "%s|", ext);
    for (char *p = token; *p; ++p)
        if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    return strstr(extensions, token) != NULL;
}

/* ZIP is a candidate selection for any ROM emulator; its member format is
 * checked by the extractor below. Do not discard it as a stale ROM first. */
static int emu_selection_allowed(const char *path, const char *extensions)
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    return emu_extension_allowed(path, extensions) ||
           (ext && strcasecmp(ext, ".zip") == 0);
}

/* Use a per-emulator sidecar for serial/network launches; otherwise retain
 * the ROM path chosen by the launcher. No ROMs are bundled in a PAX.  The
 * launcher selection is shared by all PAXs, so reject a path belonging to a
 * different emulator before it can leak into this one. */
static int emu_rom_path(const char *id, const char *extensions, char *out, size_t size)
{
    char config_path[96];
    char selected_buf[512];
    selected_buf[0] = 0;
    snprintf(config_path,sizeof(config_path),"/sd/roms/pax/%s.rom",id);
    const char *selected = _pax_svc->settings_rom_path_get();
    void *f = _pax_svc->file_open(config_path,"rb");
    if (f) {
        size_t n = _pax_svc->file_read(selected_buf,1,sizeof(selected_buf)-1,f);
        _pax_svc->file_close(f);
        selected_buf[n] = 0; selected_buf[strcspn(selected_buf,"\r\n")] = 0;
        /* The launcher sets the in-memory path immediately before starting
         * an app.  Prefer it for this launch; the sidecar is only the reboot
         * fallback.  This also prevents a stale sidecar from winning when an
         * SD write failed after the user selected a different game. */
        if ((!selected || !selected[0]) && selected_buf[0]) selected = selected_buf;
    }
    if (selected && selected[0] && !emu_selection_allowed(selected, extensions)) {
        if (_pax_svc->log_printf) {
            _pax_svc->log_printf("EMU ROM: ignoring stale selection for %s: %s\n",
                id ? id : "(none)", selected);
        }
        selected = NULL;
        if (selected_buf[0] && emu_selection_allowed(selected_buf, extensions))
            selected = selected_buf;
    }
    if (_pax_svc->log_printf) {
        _pax_svc->log_printf("EMU ROM: id=%s selected=%s extensions=%s\n",
            id ? id : "(none)", selected ? selected : "(none)",
            extensions ? extensions : "(none)");
    }
    const char *ext = selected ? strrchr(selected,'.') : NULL;
    char token[24];
    if (!ext || strlen(ext) > 16) {
        if (_pax_svc->log_printf) {
            _pax_svc->log_printf("EMU ROM: invalid selection for %s\n", id ? id : "(none)");
        }
        return -1;
    }
    snprintf(token,sizeof(token),"%s|",ext);
    for (char *p=token; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 'a'-'A';
    if (!strstr(extensions,token)) {
        /* A ZIP is a transparent container for normal ROM-based PAXs.  Do
         * not intercept native ZIP formats such as Neo Geo, whose emulator
         * needs the archive and its many ROM members directly. */
        if (strcasecmp(ext, ".zip") != 0 || strstr(extensions, ".zip|")) {
            if (_pax_svc->log_printf) {
                _pax_svc->log_printf("EMU ROM: unsupported selection %s\n", selected);
            }
            return -1;
        }
        if (!_pax_svc->file_zip_extract_first || !id || !id[0]) {
            if (_pax_svc->log_printf) {
                _pax_svc->log_printf("EMU ROM: ZIP extractor unavailable for %s\n", id ? id : "(none)");
            }
            return -1;
        }
        const int zip_result = _pax_svc->file_zip_extract_first(selected, extensions,
                "/sd/roms/pax/.zipcache", id, out, size);
        if (_pax_svc->log_printf) {
            _pax_svc->log_printf("EMU ROM: ZIP extract result=%d output=%s\n",
                zip_result, zip_result == 0 ? out : "(none)");
        }
        if (zip_result != 0) return -1;
        return 0;
    }
    if (strlen(selected) >= size) return -1;
    if (selected != out) strcpy(out,selected);
    return 0;
}

typedef struct { int64_t deadline; unsigned phase; unsigned hz; } emu_clock;
static void emu_pace(emu_clock *clock)
{
    clock->phase += 1000000;
    clock->deadline += clock->phase / clock->hz;
    clock->phase %= clock->hz;
    int64_t now = _pax_svc->get_time_us();
    if (clock->deadline > now) _pax_svc->delay_ms((clock->deadline-now)/1000);
    else if (now-clock->deadline > 50000) clock->deadline = now;
}

static int emu_quit(const pax_gamepad_state_t *pad)
{
    return pad->values[PAX_INPUT_MENU] ||
        (_pax_svc->input_l3_read && _pax_svc->input_l3_read());
}

/* Load the ROM selected by the launcher (or by the per-PAX sidecar) into
 * PSRAM.  Keeping this in the shared runtime makes the small-system PAXs
 * agree on the same SD-card convention without depending on newlib FILE
 * internals.  `extensions` uses the same lower-case ".ext|" tokens as
 * emu_rom_path(). */
static int emu_load_rom(const char *id, const char *extensions,
                        uint8_t **data_out, size_t *size_out,
                        char *path_out, size_t path_size)
{
    if (!data_out || !size_out || !path_out || path_size == 0) return -1;
    *data_out = NULL;
    *size_out = 0;

    if (emu_rom_path(id, extensions, path_out, path_size)) return -1;

    void *f = _pax_svc->file_open(path_out, "rb");
    if (!f) return -1;
    if (_pax_svc->file_seek(f, 0, 2) != 0) {
        _pax_svc->file_close(f);
        return -1;
    }
    long end = _pax_svc->file_tell(f);
    if (end <= 0 || end > 16L * 1024L * 1024L ||
        _pax_svc->file_seek(f, 0, 0) != 0) {
        _pax_svc->file_close(f);
        return -1;
    }

    uint8_t *rom = (uint8_t *)_pax_svc->mem_caps_alloc(
        (size_t)end, PAX_MEM_CAP_SPIRAM);
    if (!rom || _pax_svc->file_read(rom, 1, (size_t)end, f) != (size_t)end) {
        if (rom) _pax_svc->mem_free(rom);
        _pax_svc->file_close(f);
        *data_out = NULL;
        return -1;
    }
    _pax_svc->file_close(f);
    *data_out = rom;
    *size_out = (size_t)end;
    return 0;
}

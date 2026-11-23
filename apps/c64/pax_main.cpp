/* Commodore 64 PAX entry point.  The Frodo core is adapted from c64-go's
 * ODROID-GO port; all device I/O goes through the PAX service table. */
#define PAX_APP_SIDE 1

#include "runtime.h"
#include "psram_app.h"
#include "sysdeps.h"
#include "C64.h"
#include "Display.h"
#include "Prefs.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const app_services_t *_pax_svc;
ranctx pseudoRand;
char AppDirPath[1024];
static jmp_buf s_exit_env;

extern "C" int chdir(const char *path)
{
    (void)path;
    return -1;
}

extern "C" void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

void *operator new(size_t size) noexcept
{
    return _pax_svc ? _pax_svc->mem_alloc(size) : NULL;
}
void *operator new[](size_t size) noexcept
{
    return _pax_svc ? _pax_svc->mem_alloc(size) : NULL;
}
void operator delete(void *ptr) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}
void operator delete[](void *ptr) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}
void operator delete(void *ptr, size_t) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}
void operator delete[](void *ptr, size_t) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}

static int load_blob(const char *const *paths, size_t path_count,
                     void *dst, size_t expected, const char **used_path)
{
    for (size_t i = 0; i < path_count; ++i) {
        FILE *file = fopen(paths[i], "rb");
        if (!file) continue;
        size_t got = fread(dst, 1, expected, file);
        fclose(file);
        if (got == expected) {
            if (used_path) *used_path = paths[i];
            return 0;
        }
    }
    return -1;
}

static int load_c64_roms(C64 *c64)
{
    static const char *basic[] = {
        "/sd/roms/c64/bios/Basic ROM",
        "/sd/roms/c64/bios/Basic ROM.rom",
        "/sd/roms/c64/bios/basic.rom"
    };
    static const char *kernal[] = {
        "/sd/roms/c64/bios/Kernal ROM",
        "/sd/roms/c64/bios/Kernal ROM.rom",
        "/sd/roms/c64/bios/kernal.rom"
    };
    static const char *character[] = {
        "/sd/roms/c64/bios/Char ROM",
        "/sd/roms/c64/bios/Char ROM.rom",
        "/sd/roms/c64/bios/char.rom"
    };
    static const char *drive[] = {
        "/sd/roms/c64/bios/1541 ROM",
        "/sd/roms/c64/bios/1541 ROM.rom",
        "/sd/roms/c64/bios/1541.rom"
    };
    const char *used = NULL;

    if (load_blob(basic, sizeof(basic) / sizeof(basic[0]),
                  c64->Basic, 0x2000, &used)) {
        _pax_svc->log_printf("C64 PAX: missing 8 KiB BASIC ROM\n");
        return -1;
    }
    if (load_blob(kernal, sizeof(kernal) / sizeof(kernal[0]),
                  c64->Kernal, 0x2000, &used)) {
        _pax_svc->log_printf("C64 PAX: missing 8 KiB KERNAL ROM\n");
        return -1;
    }
    if (load_blob(character, sizeof(character) / sizeof(character[0]),
                  c64->Char, 0x1000, &used)) {
        _pax_svc->log_printf("C64 PAX: missing 4 KiB character ROM\n");
        return -1;
    }
    if (load_blob(drive, sizeof(drive) / sizeof(drive[0]),
                  c64->ROM1541, 0x4000, &used)) {
        _pax_svc->log_printf("C64 PAX: missing 16 KiB 1541 ROM\n");
        return -1;
    }
    return 0;
}

extern "C" void c64_beforeStart(void) {}
extern "C" void c64_started(void) {}

/* The original ODROID-GO port used a writable host-side temp file while
 * extracting a PRG from a T64 image.  PAX file handles are opaque, so create
 * the equivalent stream through the launcher-owned SD service. */
extern "C" FILE *_tmpfile(void)
{
    static unsigned sequence;
    if (!_pax_svc || !_pax_svc->file_open) return nullptr;
    char path[96];
    snprintf(path, sizeof(path), "/sd/roms/c64/.pax_tmp_%u", sequence++);
    return static_cast<FILE *>(_pax_svc->file_open(path, "w+b"));
}

enum {
    C64_MENU_AUTO_START_SETTING = 1,
    C64_MENU_LOAD_RUN_NOW = 2,
    C64_MENU_BACK = 3,
    C64_MENU_BUTTON_MAPPER = 4,
    C64_MENU_RIVER_RAID_PRESET = 5,
    C64_MENU_MAPPER_BACK = 6,
    C64_MENU_BINDING_BASE = 100,
};

static bool c64_auto_start_d64 = true;
static const uint8_t c64_mapper_sources[] = {
    PAX_INPUT_UP, PAX_INPUT_RIGHT, PAX_INPUT_DOWN, PAX_INPUT_LEFT,
    PAX_INPUT_A, PAX_INPUT_B, PAX_INPUT_X, PAX_INPUT_Y,
    PAX_INPUT_L, PAX_INPUT_R
};
static const char *const c64_mapper_source_names[] = {
    "DPAD UP", "DPAD RIGHT", "DPAD DOWN", "DPAD LEFT",
    "A", "B", "X", "Y", "L", "R"
};
static const char *const c64_mapper_action_names[] = {
    "MOVE UP", "MOVE DOWN", "MOVE LEFT", "MOVE RIGHT", "FIRE 1", "FIRE 2"
};
static uint8_t c64_gamepad_bindings[6] = {
    PAX_INPUT_UP, PAX_INPUT_DOWN, PAX_INPUT_LEFT, PAX_INPUT_RIGHT,
    PAX_INPUT_A, PAX_INPUT_B
};

static bool c64_mapper_source_valid(unsigned input)
{
    for (size_t i = 0; i < sizeof(c64_mapper_sources); ++i)
        if (c64_mapper_sources[i] == input) return true;
    return false;
}

static size_t c64_mapper_source_index(uint8_t input)
{
    for (size_t i = 0; i < sizeof(c64_mapper_sources); ++i)
        if (c64_mapper_sources[i] == input) return i;
    return 0;
}

static void c64_options_load(void)
{
    if (!_pax_svc || !_pax_svc->file_open || !_pax_svc->file_read || !_pax_svc->file_close)
        return;
    void *file = _pax_svc->file_open("/sd/roms/pax/c64-options.cfg", "rb");
    if (!file) return;
    char value[96] = {};
    const size_t size = _pax_svc->file_read(value, 1, sizeof(value) - 1, file);
    _pax_svc->file_close(file);
    value[size] = '\0';
    unsigned values[7] = {};
    if (sscanf(value, "%u,%u,%u,%u,%u,%u,%u", &values[0], &values[1],
               &values[2], &values[3], &values[4], &values[5], &values[6]) == 7 &&
        values[0] <= 1) {
        c64_auto_start_d64 = values[0] != 0;
        for (size_t i = 0; i < 6; ++i) {
            if (c64_mapper_source_valid(values[i + 1]))
                c64_gamepad_bindings[i] = (uint8_t)values[i + 1];
        }
    } else if (value[0] == '0') c64_auto_start_d64 = false;
    else if (value[0] == '1') c64_auto_start_d64 = true;
}

static bool c64_options_save(void)
{
    if (!_pax_svc || !_pax_svc->file_open || !_pax_svc->file_write || !_pax_svc->file_close)
        return false;
    void *file = _pax_svc->file_open("/sd/roms/pax/c64-options.cfg", "wb");
    if (!file) return false;
    char contents[96];
    const int length = snprintf(contents, sizeof(contents), "%u,%u,%u,%u,%u,%u,%u\n",
        c64_auto_start_d64 ? 1u : 0u,
        (unsigned)c64_gamepad_bindings[0], (unsigned)c64_gamepad_bindings[1],
        (unsigned)c64_gamepad_bindings[2], (unsigned)c64_gamepad_bindings[3],
        (unsigned)c64_gamepad_bindings[4], (unsigned)c64_gamepad_bindings[5]);
    if (length <= 0 || (size_t)length >= sizeof(contents)) {
        _pax_svc->file_close(file);
        return false;
    }
    const bool written = _pax_svc->file_write(contents, 1, (size_t)length, file) == (size_t)length;
    return _pax_svc->file_close(file) == 0 && written;
}

static void c64_apply_gamepad_bindings(C64Display *display)
{
    if (!display) return;
    for (uint8_t i = 0; i < 6; ++i)
        display->setGamepadBinding(i, c64_gamepad_bindings[i]);
}

static void c64_show_options(const char *status, uint32_t selected)
{
    if (!_pax_svc || !_pax_svc->game_menu_set_page) return;
    pax_game_menu_page_t page = {};
    snprintf(page.title, sizeof(page.title), "COMMODORE 64 OPTIONS");
    snprintf(page.status, sizeof(page.status), "%s", status ? status : "C64 OPTIONS");
    page.count = 4;
    page.selected = selected < page.count ? selected : 0;
    page.items[0].action = C64_MENU_AUTO_START_SETTING;
    page.items[0].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
    snprintf(page.items[0].label, sizeof(page.items[0].label), "AUTO-START D64: %s",
             c64_auto_start_d64 ? "ON" : "OFF");
    page.items[1].action = C64_MENU_LOAD_RUN_NOW;
    snprintf(page.items[1].label, sizeof(page.items[1].label), "LOAD & RUN NOW");
    page.items[2].action = C64_MENU_BUTTON_MAPPER;
    snprintf(page.items[2].label, sizeof(page.items[2].label), "BUTTON MAPPER");
    page.items[3].action = C64_MENU_BACK;
    snprintf(page.items[3].label, sizeof(page.items[3].label), "BACK TO GAME MENU");
    _pax_svc->game_menu_set_page(&page);
}

static void c64_show_mapper(const char *status, uint32_t selected)
{
    if (!_pax_svc || !_pax_svc->game_menu_set_page) return;
    pax_game_menu_page_t page = {};
    snprintf(page.title, sizeof(page.title), "C64 BUTTON MAPPER");
    snprintf(page.status, sizeof(page.status), "%s",
             status ? status : "RIVER RAID: STEER + FIRE; LEFT/RIGHT TO CHANGE");
    page.count = 8;
    page.selected = selected < page.count ? selected : 0;
    for (uint8_t i = 0; i < 6; ++i) {
        page.items[i].action = C64_MENU_BINDING_BASE + i;
        page.items[i].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
        const size_t source = c64_mapper_source_index(c64_gamepad_bindings[i]);
        snprintf(page.items[i].label, sizeof(page.items[i].label), "%s: %s",
                 c64_mapper_action_names[i], c64_mapper_source_names[source]);
    }
    page.items[6].action = C64_MENU_RIVER_RAID_PRESET;
    snprintf(page.items[6].label, sizeof(page.items[6].label), "RIVER RAID DEFAULT");
    page.items[7].action = C64_MENU_MAPPER_BACK;
    snprintf(page.items[7].label, sizeof(page.items[7].label), "BACK TO C64 OPTIONS");
    _pax_svc->game_menu_set_page(&page);
}

static void c64_game_menu_action(void *context, uint32_t action, int32_t direction)
{
    if (!_pax_svc || !_pax_svc->game_menu_set_page) return;
    if (action == C64_MENU_BACK) {
        _pax_svc->game_menu_set_page(NULL);
        return;
    }

    C64Display *display = static_cast<C64Display *>(context);
    const char *status = "CHANGE AUTO-START WITH LEFT/RIGHT";
    if (action == C64_MENU_BUTTON_MAPPER) {
        c64_show_mapper(nullptr, 0);
        return;
    }
    if (action == C64_MENU_MAPPER_BACK) {
        c64_show_options("C64 OPTIONS", 2);
        return;
    }
    if (action >= C64_MENU_BINDING_BASE && action < C64_MENU_BINDING_BASE + 6) {
        const uint8_t binding = (uint8_t)(action - C64_MENU_BINDING_BASE);
        size_t source = c64_mapper_source_index(c64_gamepad_bindings[binding]);
        const size_t source_count = sizeof(c64_mapper_sources);
        source = direction < 0 ? (source + source_count - 1) % source_count :
                                 (source + 1) % source_count;
        c64_gamepad_bindings[binding] = c64_mapper_sources[source];
        c64_apply_gamepad_bindings(display);
        const bool saved = c64_options_save();
        c64_show_mapper(saved ? "MAPPING SAVED" : "SAVE FAILED; MAPPING IS TEMPORARY", binding);
        return;
    }
    if (action == C64_MENU_RIVER_RAID_PRESET) {
        const uint8_t preset[6] = {PAX_INPUT_UP, PAX_INPUT_DOWN, PAX_INPUT_LEFT,
            PAX_INPUT_RIGHT, PAX_INPUT_A, PAX_INPUT_B};
        memcpy(c64_gamepad_bindings, preset, sizeof(preset));
        c64_apply_gamepad_bindings(display);
        const bool saved = c64_options_save();
        c64_show_mapper(saved ? "RIVER RAID MAPPING SAVED" : "SAVE FAILED; PRESET IS TEMPORARY", 6);
        return;
    }
    if (action == C64_MENU_AUTO_START_SETTING) {
        c64_auto_start_d64 = direction == 0 ? !c64_auto_start_d64 : direction > 0;
        const bool saved = c64_options_save();
        if (!c64_auto_start_d64 && display != nullptr)
            display->setAutoRun(false);  // Cancel a pending boot sequence when switching off.
        status = saved ? "SETTING SAVED; USED ON NEXT D64 LAUNCH" : "SAVE FAILED; SETTING IS TEMPORARY";
    } else if (action == C64_MENU_LOAD_RUN_NOW && display != nullptr) {
        /* Queue LOAD"*",8,1 followed by RUN through the normal BASIC keyboard
         * scanner. This can be used again for D64s and the SD directory drive. */
        display->setAutoRun(true);
        if (_pax_svc->log_printf)
            _pax_svc->log_printf("C64 PAX: menu requested auto LOAD/RUN\n");
        status = "QUEUED: LOAD* THEN RUN";
    }
    c64_show_options(status, action == C64_MENU_LOAD_RUN_NOW ? 1 : 0);
}

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAX_ABI_VERSION ||
        !svc->file_open || !svc->audio_init ||
        !svc->display_write_frame_custom) return -1;
    _pax_svc = svc;
    if (setjmp(s_exit_env)) {
        if (svc->display_clear) svc->display_clear(0);
        if (svc->display_flush) svc->display_flush();
        return -1;
    }

    char rom_path[512];
    const bool have_disk = emu_rom_path("c64", ".d64|.t64|", rom_path, sizeof(rom_path)) == 0;
    if (!have_disk) {
        // C64 is a computer, not a cartridge-only console. Start at the
        // normal BASIC prompt when no disk/tape image is selected; a later
        // LOAD command can still use the SD-backed directory drive.
        rom_path[0] = 0;
        svc->log_printf("C64 PAX: no disk selected; booting BASIC\n");
    }

    const char *ext = have_disk ? strrchr(rom_path, '.') : NULL;
    bool is_t64 = ext && (ext[1] == 't' || ext[1] == 'T');
    ThePrefs = Prefs();
    if (have_disk) {
        ThePrefs.DriveType[0] = is_t64 ? DRVTYPE_T64 : DRVTYPE_D64;
        strncpy(ThePrefs.DrivePath[0], rom_path,
                sizeof(ThePrefs.DrivePath[0]) - 1);
        ThePrefs.DrivePath[0][sizeof(ThePrefs.DrivePath[0]) - 1] = 0;
        for (int i = 1; i < 4; ++i) {
            ThePrefs.DriveType[i] = DRVTYPE_D64;
            ThePrefs.DrivePath[i][0] = 0;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            ThePrefs.DriveType[i] = DRVTYPE_DIR;
            strncpy(ThePrefs.DrivePath[i], "/sd/roms/c64",
                    sizeof(ThePrefs.DrivePath[i]) - 1);
            ThePrefs.DrivePath[i][sizeof(ThePrefs.DrivePath[i]) - 1] = 0;
        }
    }
    ThePrefs.Emul1541Proc = false;
    ThePrefs.Joystick1On = false;
    ThePrefs.Joystick2On = true;
    ThePrefs.JoystickSwap = false;
    /* Use the PAX digital SID renderer.  It submits stereo PCM through the
     * launcher audio service, which is already initialized below. */
    ThePrefs.SIDType = SIDTYPE_DIGITAL;
    ThePrefs.LimitSpeed = true;

    svc->log_printf("=== C64 PAX: %s ===\n", rom_path);
    svc->log_printf("C64 PAX: constructing Frodo\n");
    C64 *c64 = new C64();
    svc->log_printf("C64 PAX: Frodo constructed\n");
    if (!c64) return -1;

    int result = -1;
    if (!load_c64_roms(c64)) {
        c64_options_load();
        c64_apply_gamepad_bindings(c64->TheDisplay);
        c64->TheDisplay->setAutoRun(c64_auto_start_d64 && have_disk && !is_t64);
        if (svc->game_menu_register && svc->game_menu_set_page)
            svc->game_menu_register("COMMODORE 64 OPTIONS", c64_game_menu_action,
                                    c64->TheDisplay);
        svc->log_printf("C64 PAX: running Frodo with %s\n",
                        have_disk ? (is_t64 ? "T64" : "D64") : "BASIC/no disk");
        c64->Run();
        result = 0;
    }

    if (svc->game_menu_register) svc->game_menu_register(NULL, NULL, NULL);
    delete c64;
    svc->display_clear(0);
    svc->display_flush();
    svc->log_printf("=== C64 PAX exited (%d) ===\n", result);
    return result;
}

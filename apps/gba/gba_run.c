/*
 * Game Boy Advance on the launcher: gpSP (midworld's fork with an RV32 dynamic
 * recompiler, apps/gba/core) driven through its libretro interface.
 *
 * emu_main.c (the shared emulator main) picks the ROM and calls gba_run(rom).
 * This file is a small libretro frontend on the odroid layer:
 *   video  240x160 RGB565 -> ili9341_write_frame_rgb565_custom (the launcher scales it)
 *   audio  32768 Hz stereo -> odroid_audio_submit (blocks when the ring is full: paces the game)
 *   input  odroid pad: A B L R Select Start + D-pad; X or the left edge = menu, Y / right edge = volume
 *   JIT    executable PSRAM from the launcher (papp_code_alloc, MEM v2); without it gpSP interprets
 *   saves  battery save <save_dir>/<rom>.srm (written when it changes, and on exit);
 *          save state <save_dir>/<rom>.sav (the browser's Resume)
 *   BIOS   /sd/roms/gba/gba_bios.bin if present, else gpSP's open BIOS
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "papp/papp.h"
#include "libretro.h"
#include "gpsp_rv32_platform.h"
#include "odroid_audio.h"
#include "odroid_display.h"
#include "odroid_input.h"
#include "odroid_menu.h"
#include "odroid_settings.h"

#define SAVE_DIR    "/sd/odroid/data/gba"
#define SYSTEM_DIR  "/sd/roms/gba"

void app_return_to_launcher(void);
bool gpsp_set_rom_buffer_limit(unsigned mib);           /* gba_memory.c */
void gpsp_rom_page_stats(uint32_t *loads, uint32_t *reloads, uint32_t *remaps, uint32_t *cycles);

#if defined(GPSP_BENCHMARK)
/* Profiling build (GBA_BENCH=1 build.py): where a frame's time goes, summed over 5 s */
#include "gpsp_benchmark.h"
static struct { uint64_t cycles[GPSP_BENCHMARK_TIMING_BUCKET_COUNT]; uint32_t enters[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
                uint64_t instret[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
                uint64_t exec_us, audio_us, video_us; uint32_t frames; } s_bench;

#if defined(GPSP_BENCHMARK_INSTRET)
static uint32_t read_instret(void) { uint32_t n; __asm__ volatile("rdinstret %0" : "=r"(n)); return n; }
#endif

static void bench_frame(void)
{
    struct gpsp_benchmark_frame f;
    gpsp_benchmark_get_last_frame(&f);
    for (int i = 0; i < GPSP_BENCHMARK_TIMING_BUCKET_COUNT; i++) {
        s_bench.cycles[i] += f.cpu.cycles[i];
        s_bench.enters[i] += f.cpu.enters[i];
        s_bench.instret[i] += f.cpu.instret[i];
    }
    s_bench.exec_us += f.execute_us;
    s_bench.audio_us += f.audio_us;
    s_bench.video_us += f.video_us;
    s_bench.frames++;
}

static void bench_print(void)
{
    static const char *const name[GPSP_BENCHMARK_TIMING_BUCKET_COUNT] = { "cpu", "sched", "timer", "dma", "sound",
                                                                         "scanline", "vsetup" };
    if (!s_bench.frames) return;
    char line[256];
    int n = snprintf(line, sizeof(line), "GBA bench/frame: exec %u us (", (unsigned)(s_bench.exec_us / s_bench.frames));
    for (int i = 0; i < GPSP_BENCHMARK_TIMING_BUCKET_COUNT && n < (int)sizeof(line); i++)
        n += snprintf(line + n, sizeof(line) - n, "%s%s %u us x%u", i ? ", " : "", name[i],
                      (unsigned)(s_bench.cycles[i] / 360 / s_bench.frames), (unsigned)(s_bench.enters[i] / s_bench.frames));
    if (n < (int)sizeof(line))
        snprintf(line + n, sizeof(line) - n, "), audio %u us, video %u us",
                 (unsigned)(s_bench.audio_us / s_bench.frames), (unsigned)(s_bench.video_us / s_bench.frames));
    papp_log("%s\n", line);
#if defined(GPSP_BENCHMARK_INSTRET)
    papp_log("GBA bench/frame: cpu %u instructions in %u cycles (IPC x100 %u), scanline IPC x100 %u\n",
             (unsigned)(s_bench.instret[0] / s_bench.frames), (unsigned)(s_bench.cycles[0] / s_bench.frames),
             (unsigned)(s_bench.instret[0] * 100 / (s_bench.cycles[0] + 1)),
             (unsigned)(s_bench.instret[5] * 100 / (s_bench.cycles[5] + 1)));
#endif

    /* the JIT engine: per frame, from the change since the last print */
    static struct gpsp_benchmark_engine prev;
    static struct gpsp_benchmark_engine e;
    gpsp_benchmark_get_engine(&e);
    unsigned f = s_bench.frames;
#define D(x) ((unsigned)((e.x - prev.x) / f))
#define US(x) ((unsigned)((e.x - prev.x) / 360 / f))
    papp_log("GBA bench/frame: blocks %u (bios %u ewram %u iwram %u rom %u), interp steps %u runs %u; "
             "us: total %u lookup %u blocks %u interp %u\n",
             D(blocks), D(block_region.bios), D(block_region.ewram), D(block_region.iwram), D(block_region.rom),
             D(scalar_steps), D(scalar_runs), US(total_cycles), US(lookup_cycles), US(block_cycles), US(scalar_cycles));
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    papp_log("GBA bench/frame us: code %u helpers %u (x%u), memory %u (x%u: rom %u ram %u io %u other %u, stores %u); "
             "blocks bios %u ewram %u iwram %u rom %u; ram ld fast/slow %u/%u st %u/%u\n",
             US(jit_detail.code_cycles), US(jit_detail.helper_cycles), D(jit_detail.helper_calls),
             US(jit_detail.memory_cycles), D(jit_detail.memory_calls), D(jit_detail.memory_rom_loads),
             D(jit_detail.memory_ram_loads), D(jit_detail.memory_io_loads), D(jit_detail.memory_other_loads),
             D(jit_detail.memory_stores), US(jit_detail.block_cycles.bios), US(jit_detail.block_cycles.ewram),
             US(jit_detail.block_cycles.iwram), US(jit_detail.block_cycles.rom), D(jit_detail.ram_load_fast),
             D(jit_detail.ram_load_slow), D(jit_detail.ram_store_fast), D(jit_detail.ram_store_slow));
    struct gpsp_benchmark_jit_hot_sites hot;
    gpsp_benchmark_take_hot_sites(&hot);
    papp_log("GBA bench hot: %08x %u, %08x %u, %08x %u, %08x %u (of %u samples)\n",
             (unsigned)hot.top[0].pc, (unsigned)hot.top[0].samples, (unsigned)hot.top[1].pc, (unsigned)hot.top[1].samples,
             (unsigned)hot.top[2].pc, (unsigned)hot.top[2].samples, (unsigned)hot.top[3].pc, (unsigned)hot.top[3].samples,
             (unsigned)hot.samples);
#endif
#undef D
#undef US
    prev = e;
    memset(&s_bench, 0, sizeof(s_bench));
}
#endif

/* ── JIT memory ────────────────────────────────────────────────────────── */

static unsigned s_code_bytes;
static bool s_nojit;                                    /* SAVE_DIR/nojit exists: interpreter (debugging) */

static void *exec_alloc(size_t alignment, size_t size)
{
    (void)alignment;                                   /* 64 KB pages: aligned enough */
    void *p = s_nojit ? NULL : papp_code_alloc(size);
    if (p) s_code_bytes += size;
    return p;
}

static void exec_free(void *code) { papp_code_free(code); }

static bool exec_publish(void *code, size_t size) { return papp_code_sync(code, size) == PAPP_E_OK; }

static void jit_vlog(gpsp_rv32_log_level level, const char *tag, const char *fmt, va_list ap)
{
    char line[160];
    vsnprintf(line, sizeof(line), fmt, ap);
    papp_log("GBA %s%s: %s\n", level == GPSP_RV32_LOG_WARN ? "warning " : "", tag ? tag : "jit", line);
}

static const struct gpsp_rv32_platform_ops s_platform = { exec_alloc, exec_free, exec_publish, jit_vlog };

/* ── libretro callbacks ────────────────────────────────────────────────── */

static const struct { const char *key, *value; } s_options[] = {
    { "gpsp_bios", "auto" },                 /* the real BIOS if it's on the card */
    { "gpsp_boot_mode", "game" },
    { "gpsp_sound_rate", "32768" },
    { "gpsp_frameskip", "auto" },            /* skip drawing when the sound runs low (see main loop) */
    { "gpsp_color_correction", "disabled" },
    { "gpsp_frame_mixing", "disabled" },
    { "gpsp_rtc", "auto" },
    { "gpsp_rumble", "disabled" },
    { "gpsp_serial", "disabled" },
    { "gpsp_sprlim", "enabled" },
    { "gpsp_drc", "enabled" },
};

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_INFO) return;
    va_list ap;
    va_start(ap, fmt);
    char line[200];
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    papp_log("gpSP: %s", line);
    if (!line[0] || line[strlen(line) - 1] != '\n') papp_log("\n");
}

static retro_time_t perf_usec(void)                     { return papp_time_us(); }
static retro_perf_tick_t perf_counter(void)             { uint32_t c; __asm__ volatile("rdcycle %0" : "=r"(c)); return c; }
static uint64_t perf_cpu_features(void)                 { return 0; }
static void perf_log(void)                              { }
static void perf_register(struct retro_perf_counter *c) { c->registered = true; }
static void perf_start(struct retro_perf_counter *c)    { (void)c; }
static void perf_stop(struct retro_perf_counter *c)     { (void)c; }

static retro_audio_buffer_status_callback_t s_audio_status;   /* gpSP's frameskip input */
#define SKIP_BELOW_MS 30                                  /* less sound queued than this: skip drawing */

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        const struct retro_audio_buffer_status_callback *cb = data;
        s_audio_status = cb ? cb->callback : NULL;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        for (size_t i = 0; i < sizeof(s_options) / sizeof(s_options[0]); i++)
            if (!strcmp(v->key, s_options[i].key)) {
                v->value = s_options[i].value;
                return true;
            }
        v->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY ? SYSTEM_DIR : SAVE_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
        struct retro_perf_callback *p = data;
        p->get_time_usec = perf_usec;
        p->get_cpu_features = perf_cpu_features;
        p->get_perf_counter = perf_counter;
        p->perf_register = perf_register;
        p->perf_start = perf_start;
        p->perf_stop = perf_stop;
        p->perf_log = perf_log;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const struct retro_message *m = data;
        if (m && m->msg) papp_log("gpSP: %s\n", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        return true;
    default:
        return false;
    }
}

static int s_frames, s_shown;
static int64_t s_video_us, s_audio_us;                  /* time in the frame and audio calls (stats) */

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    s_frames++;
    if (!data) return;                                  /* frame skipped by the core */
    if (pitch != w * 2) return;                         /* never with gpSP (pitch 240) */
    int64_t t = papp_time_us();
    ili9341_write_frame_rgb565_custom(data, (uint16_t)w, (uint16_t)h, 1.0f, false);
    s_video_us += papp_time_us() - t;
    s_shown++;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    int64_t t = papp_time_us();
    odroid_audio_submit((short *)data, (int)frames);
    s_audio_us += papp_time_us() - t;
    return frames;
}

static void audio_sample(int16_t l, int16_t r)
{
    int16_t s[2] = { l, r };
    odroid_audio_submit(s, 1);
}

static uint16_t s_buttons;                              /* RETRO_DEVICE_ID_JOYPAD_* bits */

static void input_poll(void) { }

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)s_buttons;
    return id < 16 ? (s_buttons >> id) & 1 : 0;
}

/* ── Saves ─────────────────────────────────────────────────────────────── */

static char s_srm[300], s_state[300];
static uint8_t *s_srm_copy;                             /* what's on the card */
static size_t s_srm_size;

static void save_paths(const char *rom)
{
    const char *base = strrchr(rom, '/');
    base = base ? base + 1 : rom;
    snprintf(s_srm, sizeof(s_srm), SAVE_DIR "/%s.srm", base);
    snprintf(s_state, sizeof(s_state), SAVE_DIR "/%s.sav", base);
    mkdir("/sd/odroid", 0775);
    mkdir("/sd/odroid/data", 0775);
    mkdir(SAVE_DIR, 0775);
}

static void srm_load(void)
{
    uint8_t *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    s_srm_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !s_srm_size) return;
    FILE *f = fopen(s_srm, "rb");
    if (f) {
        size_t n = fread(mem, 1, s_srm_size, f);
        fclose(f);
        papp_log("GBA: battery save %s (%u bytes)\n", s_srm, (unsigned)n);
    }
    s_srm_copy = malloc(s_srm_size);
    if (s_srm_copy) memcpy(s_srm_copy, mem, s_srm_size);
}

/* Writes the battery save if the game changed it */
static void srm_flush(void)
{
    uint8_t *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size) return;
    if (s_srm_copy && size == s_srm_size && !memcmp(mem, s_srm_copy, size)) return;
    FILE *f = fopen(s_srm, "wb");
    if (!f) return;
    size_t n = fwrite(mem, 1, size, f);
    fclose(f);
    if (n == size && s_srm_copy && size == s_srm_size) memcpy(s_srm_copy, mem, size);
    papp_log("GBA: battery save written (%u bytes)\n", (unsigned)n);
}

static bool state_save(void)
{
    size_t size = retro_serialize_size();
    void *buf = size ? malloc(size) : NULL;
    bool ok = buf && retro_serialize(buf, size);
    FILE *f = ok ? fopen(s_state, "wb") : NULL;
    ok = f && fwrite(buf, 1, size, f) == size;
    if (f) fclose(f);
    free(buf);
    return ok;
}

static bool state_load(void)
{
    size_t size = retro_serialize_size();
    void *buf = size ? malloc(size) : NULL;
    FILE *f = buf ? fopen(s_state, "rb") : NULL;
    bool ok = f && fread(buf, 1, size, f) == size;
    if (f) fclose(f);
    ok = ok && retro_unserialize(buf, size);
    free(buf);
    return ok;
}

static bool state_exists(void)
{
    struct stat st;
    return stat(s_state, &st) == 0;
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

static void quit(void)
{
    srm_flush();
    free(s_srm_copy);
    s_srm_copy = NULL;
    retro_unload_game();
    retro_deinit();
    app_return_to_launcher();
}

/* The in-game menu; true: leave the frame loop (never, quit doesn't return) */
static void menu(void)
{
    const char *note = NULL;
    for (;;) {
        switch (odroid_emu_menu(note, state_exists() ? EMU_MENU_HAS_SAVE : 0)) {
        case EMU_MENU_SAVE:    note = state_save() ? "saved" : "save failed"; continue;
        case EMU_MENU_DELETE:  note = remove(s_state) == 0 ? "save deleted" : "delete failed"; continue;
        case EMU_MENU_LOAD:    if (!state_load()) papp_log("GBA: load failed\n"); return;
        case EMU_MENU_RESTART: retro_reset(); return;
        case EMU_MENU_EXIT:    quit(); return;
        default:               return;
        }
    }
}

/* Logs the cartridge header; false if it isn't a GBA ROM (no Nintendo logo) */
static bool rom_header_ok(const char *rom)
{
    static const uint8_t logo[8] = { 0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21 };
    uint8_t h[0xC0];
    FILE *f = fopen(rom, "rb");
    bool ok = f && fread(h, 1, sizeof(h), f) == sizeof(h);
    if (f) fclose(f);
    if (!ok) return false;
    char title[13], code[5];
    for (int i = 0; i < 12; i++) title[i] = h[0xA0 + i] >= 32 && h[0xA0 + i] < 127 ? (char)h[0xA0 + i] : '.';
    for (int i = 0; i < 4; i++) code[i] = h[0xAC + i] >= 32 && h[0xAC + i] < 127 ? (char)h[0xAC + i] : '.';
    title[12] = code[4] = 0;
    ok = !memcmp(h + 4, logo, sizeof(logo));
    papp_log("GBA: cartridge \"%s\" code %s%s\n", title, code, ok ? "" : " - not a GBA ROM (no Nintendo logo)");
    return ok;
}

void gba_run(const char *rom)
{
    save_paths(rom);
    rom_header_ok(rom);
    struct stat st;
    s_nojit = stat(SAVE_DIR "/nojit", &st) == 0;
    /* gpSP keeps up to this many MB of the ROM in PSRAM and pages the rest from the card;
     * leave room for the JIT caches (up to 4 MB) and the rest of the app */
    size_t free_psram = papp_mem_free_size(PAPP_MEM_LARGE);
    unsigned rom_mib = free_psram > (9u << 20) ? (unsigned)((free_psram - (7u << 20)) >> 20) : 2;
    if (rom_mib > 32) rom_mib = 32;
    FILE *lf = fopen(SAVE_DIR "/romlimit", "r");          /* debugging: force paging */
    if (lf) {
        char num[8] = { 0 };
        fread(num, 1, sizeof(num) - 1, lf);
        fclose(lf);
        unsigned v = (unsigned)atoi(num);
        if (v >= 1 && v < rom_mib) rom_mib = v;
    }
    gpsp_set_rom_buffer_limit(rom_mib);
    extern uint32_t gpsp_frameskip_max;
    lf = fopen(SAVE_DIR "/maxskip", "r");                 /* debugging: frames skipped in a row at most */
    if (lf) {
        char num[8] = { 0 };
        fread(num, 1, sizeof(num) - 1, lf);
        fclose(lf);
        gpsp_frameskip_max = (uint32_t)atoi(num);
        papp_log("GBA: frameskip at most %u in a row\n", (unsigned)gpsp_frameskip_max);
    }
    papp_log("GBA: ROM buffer up to %u MB (PSRAM free %u KB)\n", rom_mib, (unsigned)(free_psram >> 10));
    gpsp_rv32_set_platform(&s_platform);
#if defined(GPSP_BENCHMARK_INSTRET)
    gpsp_benchmark_set_instret_counter(read_instret);
#endif
    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_game_info info = { .path = rom };
    int64_t t0 = papp_time_us();
    if (!retro_load_game(&info)) {
        papp_log("GBA: cannot load %s\n", rom);
        retro_deinit();
        app_return_to_launcher();
        return;
    }
    papp_log("GBA: loaded in %d ms; JIT code memory %u KB%s\n", (int)((papp_time_us() - t0) / 1000),
             s_code_bytes / 1024, s_code_bytes ? "" : " (none: interpreter)");
    srm_load();
    if (odroid_settings_StartAction_get() == ODROID_START_ACTION_RESTART && !state_load())
        papp_log("GBA: no save state to resume\n");

    odroid_audio_init(32768);
    int64_t stat_t = papp_time_us(), srm_t = stat_t;
    int stat_frames = 0;
    for (;;) {
        if (papp_exit_requested()) quit();
        odroid_gamepad_state pad;
        odroid_input_gamepad_read(&pad);
        if (pad.values[ODROID_INPUT_MENU]) { menu(); continue; }
        if (pad.values[ODROID_INPUT_VOLUME]) { odroid_emu_volume(); continue; }
        uint16_t b = 0;
        if (pad.values[ODROID_INPUT_A]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
        if (pad.values[ODROID_INPUT_B]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
        if (pad.values[ODROID_INPUT_SELECT]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
        if (pad.values[ODROID_INPUT_START]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
        if (pad.values[ODROID_INPUT_UP]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
        if (pad.values[ODROID_INPUT_DOWN]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
        if (pad.values[ODROID_INPUT_LEFT]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
        if (pad.values[ODROID_INPUT_RIGHT]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
        if (pad.values[ODROID_INPUT_L]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_L;
        if (pad.values[ODROID_INPUT_R]) b |= 1 << RETRO_DEVICE_ID_JOYPAD_R;
        s_buttons = b;

        if (s_audio_status) {
            int ms = odroid_audio_queued_ms();
            if (ms >= 0) s_audio_status(true, (unsigned)(ms > 100 ? 100 : ms), ms < SKIP_BELOW_MS);
        }
        retro_run();
#if defined(GPSP_BENCHMARK)
        bench_frame();
#endif
        stat_frames++;

        int64_t now = papp_time_us();
        if (now - stat_t >= 5000000) {
            extern uint32_t rv32_ram_cache_flush_count, rv32_rom_cache_flush_count;
            static uint32_t last_loads, last_cycles, last_ram_fl, last_rom_fl;
            uint32_t loads, reloads, remaps, cycles;
            gpsp_rom_page_stats(&loads, &reloads, &remaps, &cycles);
            papp_log("GBA: %.1f fps (%d shown), video %d ms, audio wait %d ms, ROM pages read %u (%u ms), "
                     "JIT flushes RAM %u ROM %u\n",
                     stat_frames * 1e6 / (double)(now - stat_t), s_shown, (int)(s_video_us / 1000),
                     (int)(s_audio_us / 1000), (unsigned)(loads - last_loads),
                     (unsigned)((cycles - last_cycles) / 360000),
                     (unsigned)(rv32_ram_cache_flush_count - last_ram_fl),
                     (unsigned)(rv32_rom_cache_flush_count - last_rom_fl));
            last_ram_fl = rv32_ram_cache_flush_count;
            last_rom_fl = rv32_rom_cache_flush_count;
            s_video_us = s_audio_us = 0;
            last_loads = loads;
            last_cycles = cycles;
            stat_t = now;
            stat_frames = 0;
#if defined(GPSP_BENCHMARK)
            bench_print();
#endif
            s_shown = 0;
        }
        if (now - srm_t >= 3000000) {                   /* games write their save over a few frames */
            srm_t = now;
            srm_flush();
        }
    }
}

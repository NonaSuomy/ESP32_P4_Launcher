/*
 * SD Card Speed Test: sequential and random benchmarks for the mounted SD card.
 * Uses one private temporary file, verifies the readback, and removes the file.
 */
#include <stdio.h>
#include <string.h>
#include "papp/papp.h"
#include "edu_ui.h"

PAPP_MANIFEST(.id = "tool.sd_speedtest", .name = "SD Card Speed Test",
              .version = PAPP_APP_VERSION, .main_stack = 12288);

#define TEST_PATH "/sd/data/tool.sd_speedtest/benchmark.tmp"
#define TEST_BYTES_QUICK (8u * 1024u * 1024u)
#define TEST_BYTES_LARGE (64u * 1024u * 1024u)
#define SEQ_LARGE_BYTES (1024u * 1024u)
#define IO_4K_BYTES 4096u
#define RANDOM_OPS 256u
#define MAX_TEST_PASSES 3u
#define MAX_TEST_BLOCKS (TEST_BYTES_LARGE / IO_4K_BYTES)

enum { ST_IDLE, ST_WRITE, ST_READ, ST_DONE, ST_ERROR, ST_CANCELLED };
enum { ACT_RUN = 1, ACT_SIZE, ACT_PASSES };

typedef struct {
    int state;
    int progress;                 /* 0..1000 */
    double write_mb_s[3];
    double read_mb_s[3];
    uint32_t verified_bytes;
    papp_fs_volume_info_t volume;
    bool volume_valid;
    char error[80];
} screen_state_t;

static screen_state_t s_result = { .state = ST_IDLE };
static papp_mutex_t *s_lock;
static papp_thread_t *s_worker;
static papp_surface_t *s_surface;
static volatile int s_cancel;
static volatile int s_quit;
static uint32_t s_prev_buttons;
static bool s_prev_enter;
static uint32_t s_test_bytes = TEST_BYTES_QUICK;
static unsigned s_test_passes = MAX_TEST_PASSES;

static screen_state_t snapshot(void);

static void update_state(int state, int progress)
{
    papp_mutex_lock(s_lock);
    s_result.state = state;
    s_result.progress = progress;
    papp_mutex_unlock(s_lock);
}

static void update_progress(int state, unsigned stage, unsigned done, unsigned total,
                            unsigned pass_count)
{
    int progress = (int)(((uint64_t)stage * 1000u + (uint64_t)done * 1000u / total) /
                         (6u * pass_count));
    update_state(state, progress);
}

static void save_rate(unsigned test, bool write, double rate)
{
    papp_mutex_lock(s_lock);
    (write ? s_result.write_mb_s : s_result.read_mb_s)[test] = rate;
    papp_mutex_unlock(s_lock);
}

static void set_error(const char *message)
{
    papp_mutex_lock(s_lock);
    snprintf(s_result.error, sizeof(s_result.error), "%s", message);
    s_result.state = ST_ERROR;
    papp_mutex_unlock(s_lock);
}

static uint8_t expected_byte(uint32_t offset)
{
    return (uint8_t)((offset * 37u) ^ ((offset >> 8) * 13u) ^ 0xA5u);
}

static void fill_expected(uint8_t *buffer, size_t count, uint32_t offset)
{
    for (size_t i = 0; i < count; i++) buffer[i] = expected_byte(offset + (uint32_t)i);
}

static bool cancelled(void)
{
    return s_cancel || papp_exit_requested();
}

static const char *fs_type_name(uint32_t type)
{
    switch (type) {
    case PAPP_FS_TYPE_FAT12: return "FAT12";
    case PAPP_FS_TYPE_FAT16: return "FAT16";
    case PAPP_FS_TYPE_FAT32: return "FAT32";
    case PAPP_FS_TYPE_EXFAT: return "exFAT";
    default: return "unknown FS";
    }
}

static void format_size(uint64_t bytes, char *out, size_t out_size)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    double amount = (double)bytes;
    unsigned unit = 0;
    while (amount >= 1000.0 && unit < 4) { amount /= 1000.0; unit++; }
    snprintf(out, out_size, "%.1f %s", amount, units[unit]);
}

static void refresh_volume_info(void)
{
    papp_fs_volume_info_t info = { 0 };
    const papp_if_fs_t *fs = papp_fs();
    bool valid = fs && fs->hdr.version >= 3 && fs->volume_info &&
                 fs->volume_info("/sd", &info) == 0;
    papp_mutex_lock(s_lock);
    s_result.volume = info;
    s_result.volume_valid = valid;
    papp_mutex_unlock(s_lock);
}

static bool benchmark_pass(const papp_if_fs_t *fs, uint8_t *buffer, uint32_t *order,
                           uint32_t test_bytes, unsigned repeat, unsigned pass_count,
                           double read_rate[3], double write_rate[3])
{
    papp_file_t *file = NULL;
    bool have_file = false;
    bool ok = false;
    bool was_cancelled = false;
    const char *error = NULL;
    const unsigned stage_base = repeat * 6u;
    int64_t read_elapsed, write_elapsed, started;

#define CHECK_CANCEL() do { if (cancelled()) { was_cancelled = true; goto cleanup; } } while (0)
#define FAIL(message) do { error = (message); goto cleanup; } while (0)

    /* SEQ 1 MiB, one synchronous request at a time (the PAPP FS API has no queued I/O). */
    fill_expected(buffer, 64u * 1024u, 0);
    for (size_t i = 64u * 1024u; i < SEQ_LARGE_BYTES; i += 64u * 1024u)
        memcpy(buffer + i, buffer, 64u * 1024u);
    update_state(ST_WRITE, 0);
    file = fs->open(TEST_PATH, "wb");
    if (!file) FAIL("Could not create the temporary test file");
    have_file = true;
    have_file = true;
    started = papp_time_us();
    for (uint32_t offset = 0; offset < test_bytes; offset += SEQ_LARGE_BYTES) {
        CHECK_CANCEL();
        if (fs->write(file, buffer, SEQ_LARGE_BYTES) != SEQ_LARGE_BYTES) FAIL("SD write failed or was incomplete");
        update_progress(ST_WRITE, stage_base + 0u, offset + SEQ_LARGE_BYTES, test_bytes, pass_count);
    }
    if (fs->close(file) != 0) { file = NULL; FAIL("Could not finish writing the test file"); }
    file = NULL;
    write_rate[0] = (double)test_bytes / (double)(papp_time_us() - started);

    file = fs->open(TEST_PATH, "rb");
    if (!file) FAIL("Could not reopen the test file for reading");
    read_elapsed = 0;
    for (uint32_t offset = 0; offset < test_bytes; offset += SEQ_LARGE_BYTES) {
        CHECK_CANCEL();
        int64_t t0 = papp_time_us();
        size_t got = fs->read(file, buffer, SEQ_LARGE_BYTES);
        read_elapsed += papp_time_us() - t0;
        if (got != SEQ_LARGE_BYTES) FAIL("SD read failed or ended early");
        for (size_t i = 0; i < SEQ_LARGE_BYTES; i++)
            if (buffer[i] != expected_byte(offset + (uint32_t)i)) FAIL("Readback check failed; card data differs");
        update_progress(ST_READ, stage_base + 1u, offset + SEQ_LARGE_BYTES, test_bytes, pass_count);
    }
    if (fs->close(file) != 0) { file = NULL; FAIL("Could not close the read test file"); }
    file = NULL;
    read_rate[0] = (double)test_bytes / (double)read_elapsed;

    /* SEQ 4 KiB, one synchronous request at a time. */
    file = fs->open(TEST_PATH, "wb");
    if (!file) FAIL("Could not reopen the test file for 4 KiB writes");
    write_elapsed = 0;
    for (uint32_t offset = 0; offset < test_bytes; offset += IO_4K_BYTES) {
        CHECK_CANCEL();
        fill_expected(buffer, IO_4K_BYTES, offset);
        int64_t t0 = papp_time_us();
        if (fs->write(file, buffer, IO_4K_BYTES) != IO_4K_BYTES) FAIL("4 KiB sequential write failed");
        write_elapsed += papp_time_us() - t0;
        if ((offset & 0x1FFFFu) == 0) update_progress(ST_WRITE, stage_base + 2u, offset + IO_4K_BYTES, test_bytes, pass_count);
    }
    int64_t close_started = papp_time_us();
    if (fs->close(file) != 0) { file = NULL; FAIL("Could not close the 4 KiB write test"); }
    write_elapsed += papp_time_us() - close_started;
    file = NULL;
    write_rate[1] = (double)test_bytes / (double)write_elapsed;

    file = fs->open(TEST_PATH, "rb");
    if (!file) FAIL("Could not reopen the test file for 4 KiB reads");
    read_elapsed = 0;
    for (uint32_t offset = 0; offset < test_bytes; offset += IO_4K_BYTES) {
        CHECK_CANCEL();
        int64_t t0 = papp_time_us();
        size_t got = fs->read(file, buffer, IO_4K_BYTES);
        read_elapsed += papp_time_us() - t0;
        if (got != IO_4K_BYTES) FAIL("4 KiB sequential read failed");
        for (size_t i = 0; i < IO_4K_BYTES; i++)
            if (buffer[i] != expected_byte(offset + (uint32_t)i)) FAIL("4 KiB readback differs");
        if ((offset & 0x1FFFFu) == 0) update_progress(ST_READ, stage_base + 3u, offset + IO_4K_BYTES, test_bytes, pass_count);
    }
    if (fs->close(file) != 0) { file = NULL; FAIL("Could not close the 4 KiB read test"); }
    file = NULL;
    read_rate[1] = (double)test_bytes / (double)read_elapsed;

    /* RND 4 KiB, 256 scattered requests over the selected test file. */
    file = fs->open(TEST_PATH, "rb");
    if (!file) FAIL("Could not reopen the test file for random reads");
    read_elapsed = 0;
    for (unsigned i = 0; i < RANDOM_OPS; i++) {
        CHECK_CANCEL();
        uint32_t offset = order[i] * IO_4K_BYTES;
        int64_t t0 = papp_time_us();
        int seek_error = fs->seek(file, (long)offset, SEEK_SET);
        size_t got = seek_error == 0 ? fs->read(file, buffer, IO_4K_BYTES) : 0;
        read_elapsed += papp_time_us() - t0;
        if (got != IO_4K_BYTES) FAIL("Random 4 KiB read failed");
        for (size_t j = 0; j < IO_4K_BYTES; j++)
            if (buffer[j] != expected_byte(offset + (uint32_t)j)) FAIL("Random readback differs");
        update_progress(ST_READ, stage_base + 4u, i + 1u, RANDOM_OPS, pass_count);
    }
    if (fs->close(file) != 0) { file = NULL; FAIL("Could not close random read test"); }
    file = NULL;
    read_rate[2] = (double)(RANDOM_OPS * IO_4K_BYTES) / (double)read_elapsed;

    file = fs->open(TEST_PATH, "r+b");
    if (!file) FAIL("Could not open the test file for random writes");
    write_elapsed = 0;
    for (unsigned i = 0; i < RANDOM_OPS; i++) {
        CHECK_CANCEL();
        uint32_t offset = order[i] * IO_4K_BYTES;
        fill_expected(buffer, IO_4K_BYTES, offset);
        int64_t t0 = papp_time_us();
        if (fs->seek(file, (long)offset, SEEK_SET) != 0 || fs->write(file, buffer, IO_4K_BYTES) != IO_4K_BYTES)
            FAIL("Random 4 KiB write failed");
        write_elapsed += papp_time_us() - t0;
        update_progress(ST_WRITE, stage_base + 5u, i + 1u, RANDOM_OPS, pass_count);
    }
    close_started = papp_time_us();
    if (fs->close(file) != 0) { file = NULL; FAIL("Could not close random write test"); }
    write_elapsed += papp_time_us() - close_started;
    file = NULL;
    write_rate[2] = (double)(RANDOM_OPS * IO_4K_BYTES) / (double)write_elapsed;

    /* Verify the full final file after the scattered writes. */
    file = fs->open(TEST_PATH, "rb");
    if (!file) FAIL("Could not reopen the test file for final verification");
    for (uint32_t offset = 0; offset < test_bytes; offset += SEQ_LARGE_BYTES) {
        CHECK_CANCEL();
        size_t got = fs->read(file, buffer, SEQ_LARGE_BYTES);
        if (got != SEQ_LARGE_BYTES) FAIL("Final SD readback was incomplete");
        for (size_t i = 0; i < SEQ_LARGE_BYTES; i++)
            if (buffer[i] != expected_byte(offset + (uint32_t)i)) FAIL("Final SD verification failed");
    }
    if (fs->close(file) != 0) { file = NULL; FAIL("Could not close final verification file"); }
    file = NULL;
    ok = true;

cleanup:
    if (file) fs->close(file);
    if (have_file && fs->remove(TEST_PATH) != 0 && !error && !was_cancelled)
        error = "Cleanup failed; benchmark.tmp remains in the app data folder";
    if (error) set_error(error);
    if (was_cancelled) update_state(ST_CANCELLED, 0);
#undef CHECK_CANCEL
#undef FAIL
    return ok && !error && !was_cancelled;
}

static int benchmark_worker(void *unused)
{
    (void)unused;
    const papp_if_fs_t *fs = papp_fs();
    uint8_t *buffer = papp_alloc(SEQ_LARGE_BYTES, PAPP_MEM_LARGE, 64);
    uint32_t *order = papp_alloc(MAX_TEST_BLOCKS * sizeof(*order), PAPP_MEM_LARGE, 4);
    uint32_t test_bytes = s_test_bytes;
    unsigned pass_count = s_test_passes;
    int final_state = ST_ERROR;

    if (!fs || !fs->open || !fs->close || !fs->read || !fs->write || !fs->seek ||
        !fs->remove || !fs->mkdir) {
        set_error("SD filesystem is unavailable");
        goto done;
    }
    if (!buffer || !order) {
        set_error("Not enough memory for test buffer");
        goto done;
    }

    (void)fs->mkdir("/sd/data");
    (void)fs->mkdir("/sd/data/tool.sd_speedtest");
    (void)fs->remove(TEST_PATH);
    refresh_volume_info();
    screen_state_t initial = snapshot();
    if (initial.volume_valid && initial.volume.free_bytes < (uint64_t)test_bytes + 1024u * 1024u) {
        set_error("Not enough free SD space for this test size");
        goto done;
    }

    unsigned blocks = test_bytes / IO_4K_BYTES;
    for (unsigned repeat = 0; repeat < pass_count; repeat++) {
        for (unsigned i = 0; i < blocks; i++) order[i] = i;
        uint32_t seed = 0x51DCA4D3u ^ repeat;
        for (unsigned i = blocks - 1; i > 0; i--) {
            seed = seed * 1664525u + 1013904223u;
            unsigned j = seed % (i + 1u);
            uint32_t t = order[i]; order[i] = order[j]; order[j] = t;
        }
        double read_rate[3] = { 0 }, write_rate[3] = { 0 };
        if (!benchmark_pass(fs, buffer, order, test_bytes, repeat, pass_count,
                            read_rate, write_rate)) {
            final_state = cancelled() ? ST_CANCELLED : ST_ERROR;
            goto finish;
        }
        for (unsigned test = 0; test < 3; test++) {
            screen_state_t current = snapshot();
            double divisor = (double)(repeat + 1u);
            save_rate(test, false, (current.read_mb_s[test] * repeat + read_rate[test]) / divisor);
            save_rate(test, true, (current.write_mb_s[test] * repeat + write_rate[test]) / divisor);
        }
        papp_mutex_lock(s_lock);
        s_result.verified_bytes = test_bytes;
        papp_mutex_unlock(s_lock);
    }
    final_state = ST_DONE;

finish:
    refresh_volume_info();
    papp_mutex_lock(s_lock);
    s_result.state = final_state;
    if (final_state == ST_DONE || final_state == ST_CANCELLED) s_result.error[0] = 0;
    papp_mutex_unlock(s_lock);

done:
    if (buffer) papp_free(buffer);
    if (order) papp_free(order);
    return final_state == ST_ERROR ? 1 : 0;
}

static screen_state_t snapshot(void)
{
    screen_state_t copy;
    papp_mutex_lock(s_lock);
    copy = s_result;
    papp_mutex_unlock(s_lock);
    return copy;
}

static bool is_running(int state)
{
    return state == ST_WRITE || state == ST_READ;
}

static void act(int id, int x)
{
    (void)x;
    if (id == UI_ID_CLOSE) { s_quit = true; return; }
    if (id == ACT_SIZE) {
        screen_state_t r = snapshot();
        if (!is_running(r.state)) s_test_bytes = s_test_bytes == TEST_BYTES_QUICK ? TEST_BYTES_LARGE : TEST_BYTES_QUICK;
        return;
    }
    if (id == ACT_PASSES) {
        screen_state_t r = snapshot();
        if (!is_running(r.state)) s_test_passes = s_test_passes == 1u ? MAX_TEST_PASSES : 1u;
        return;
    }
    if (id == ACT_RUN) {
        screen_state_t r = snapshot();
        if (is_running(r.state)) { s_cancel = 1; return; }
        if (s_worker) {
            papp_thread_join(s_worker, PAPP_WAIT_FOREVER, NULL);
            s_worker = NULL;
        }
        s_cancel = 0;
        papp_mutex_lock(s_lock);
        memset(&s_result, 0, sizeof(s_result));
        s_result.state = ST_WRITE;
        papp_mutex_unlock(s_lock);
        s_worker = papp_thread_create(benchmark_worker, NULL,
                                      &(papp_thread_cfg_t){ .name = "sd_speedtest",
                                          .stack_size = 12288, .prio = PAPP_PRIO_NORMAL, .core = -1 });
        if (!s_worker) set_error("Could not start the benchmark worker");
    }
}

static const char *state_label(const screen_state_t *r)
{
    static char ready[80];
    switch (r->state) {
    case ST_WRITE: return "Writing SD benchmark data…";
    case ST_READ: return "Reading and verifying SD data…";
    case ST_DONE: return "Readback verified; temporary file removed";
    case ST_ERROR: return r->error[0] ? r->error : "Benchmark failed";
    case ST_CANCELLED: return "Cancelled; temporary file cleaned up";
    default:
        snprintf(ready, sizeof(ready), "Ready • %lu MiB • %s • seq/random 4 KiB I/O",
                 (unsigned long)(s_test_bytes / (1024u * 1024u)),
                 s_test_passes == 1u ? "single pass" : "3-run average");
        return ready;
    }
}

static const char *sd_bus_label(void)
{
    const char *board = papp_board_id();
    /* The AIO exposes only D0 and mounts at SDMMC_FREQ_DEFAULT (20 MHz). */
    if (board && strcmp(board, "elecrow_p4_aio") == 0) return "Elecrow AIO • SDMMC 1-bit • 20 MHz";
    return "Board-specific SD bus mode; see launcher device info";
}

static void draw_screen(void)
{
    screen_state_t r = snapshot();
    char value[48], total[24], free_space[24], volume_line[112], size_label[48], passes_label[48];
    static const char *const names[] = {
        "Sequential 1 MiB (Q1T1)",
        "Sequential 4 KiB (Q1T1)",
        "Random 4 KiB (256 requests)",
    };
    ui_begin(s_surface);
    ui_bar("SD Card Speed Test", NULL, 0, 0, state_label(&r),
           r.state == ST_ERROR ? C_RED : r.state == ST_DONE ? C_GREEN : C_BLUE, false);
    if (r.volume_valid) {
        format_size(r.volume.total_bytes, total, sizeof(total));
        format_size(r.volume.free_bytes, free_space, sizeof(free_space));
        snprintf(volume_line, sizeof(volume_line), "SD volume: %s total • %s free • %s",
                 total, free_space, fs_type_name(r.volume.type));
    } else {
        strlcpy(volume_line, "Update launcher to show SD capacity and free space", sizeof(volume_line));
    }
    ui_text_c(ui_small, 400, 111, volume_line, C_DIM);
    ui_text_c(ui_small, 400, 136, sd_bus_label(), C_DIM);
    ui_text_l(ui_small, 60, 164, "Test profile", C_FAINT);
    ui_text_c(ui_small, 482, 164, "Read (MB/s)", C_DIM);
    ui_text_c(ui_small, 684, 164, "Write (MB/s)", C_DIM);
    for (unsigned i = 0; i < 3; i++) {
        int y = 178 + (int)i * 48;
        ui_rrect(40, y, 720, 40, 8, C_PANEL);
        ui_text_l(ui_small, 58, y + 20, names[i], C_TEXT);
        if (r.read_mb_s[i] > 0.0) snprintf(value, sizeof(value), "%.2f", r.read_mb_s[i]);
        else snprintf(value, sizeof(value), "—");
        ui_text_c(ui_small, 482, y + 20, value, C_GREEN);
        if (r.write_mb_s[i] > 0.0) snprintf(value, sizeof(value), "%.2f", r.write_mb_s[i]);
        else snprintf(value, sizeof(value), "—");
        ui_text_c(ui_small, 684, y + 20, value, C_OUT);
    }

    snprintf(size_label, sizeof(size_label), "Size: %lu MiB (Left/Right)",
             (unsigned long)(s_test_bytes / (1024u * 1024u)));
    snprintf(passes_label, sizeof(passes_label), "Runs: %s (Up)",
             s_test_passes == 1u ? "1 pass" : "3-pass average");
    ui_button(80, 326, 310, 34, size_label, C_RAISED, ACT_SIZE);
    ui_button(410, 326, 310, 34, passes_label, C_RAISED, ACT_PASSES);
    ui_rrect(48, 370, 632, 16, 8, C_RAISED);
    int filled = (r.progress * 632) / 1000;
    if (filled > 0) ui_rrect(48, 370, filled, 16, 8, C_BLUE);
    snprintf(value, sizeof(value), "%d%%", r.progress / 10);
    ui_text_r(ui_small, 752, 378, value, C_TEXT);

    ui_button(292, 402, 216, 48, is_running(r.state) ? "Cancel" : "Run Test",
              is_running(r.state) ? C_RED : C_BLUE, ACT_RUN);
    ui_text_c(ui_small, 400, 460, "A/Touch: Run • ←/→ Size • ↑ Passes • B Cancel • X Exit", C_DIM);
    ui_end(s_surface);
}

int main(void)
{
    s_lock = papp_mutex_create(false);
    if (!s_lock) { papp_ui_message("SD Card Speed Test", "Could not create the result lock."); return 1; }
    s_surface = ui_open();
    if (!s_surface) {
        papp_mutex_destroy(s_lock);
        papp_ui_message("SD Card Speed Test", "This app needs a display.");
        return 1;
    }
    papp_input_flush();
    refresh_volume_info();
    papp_gamepad_t pad = { 0 };
    if (papp_input_gamepad(0, &pad)) s_prev_buttons = pad.buttons;
    ui_touch_reset();

    while (!papp_exit_requested() && !s_quit) {
        ui_touch(s_surface, act);
        uint32_t buttons = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
        uint32_t pressed = buttons & ~s_prev_buttons;
        s_prev_buttons = buttons;
        if (pressed & PAPP_BTN_A) act(ACT_RUN, 0);
        if (pressed & PAPP_BTN_B) s_cancel = 1;
        if (pressed & PAPP_BTN_X) s_quit = 1;
        if (pressed & (PAPP_BTN_LEFT | PAPP_BTN_RIGHT)) act(ACT_SIZE, 0);
        if (pressed & PAPP_BTN_UP) act(ACT_PASSES, 0);
        bool enter = papp_input_key_down(0x28);
        if (enter && !s_prev_enter) act(ACT_RUN, 0);
        s_prev_enter = enter;

        screen_state_t r = snapshot();
        if (s_worker && !is_running(r.state)) {
            papp_thread_join(s_worker, PAPP_WAIT_FOREVER, NULL);
            s_worker = NULL;
        }
        draw_screen();
        papp_delay_ms(30);
    }

    s_cancel = 1;
    if (s_worker) papp_thread_join(s_worker, PAPP_WAIT_FOREVER, NULL);
    papp_disp_surface_destroy(s_surface);
    papp_mutex_destroy(s_lock);
    return 0;
}

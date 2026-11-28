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
#define TEST_BYTES (8u * 1024u * 1024u)
#define SEQ_LARGE_BYTES (1024u * 1024u)
#define IO_4K_BYTES 4096u
#define RANDOM_OPS 256u
#define TEST_BLOCKS (TEST_BYTES / IO_4K_BYTES)

enum { ST_IDLE, ST_WRITE, ST_READ, ST_DONE, ST_ERROR, ST_CANCELLED };
enum { ACT_RUN = 1 };

typedef struct {
    int state;
    int progress;                 /* 0..1000 */
    double write_mb_s[3];
    double read_mb_s[3];
    uint32_t verified_bytes;
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

static void update_state(int state, int progress)
{
    papp_mutex_lock(s_lock);
    s_result.state = state;
    s_result.progress = progress;
    papp_mutex_unlock(s_lock);
}

static void update_progress(int state, unsigned stage, unsigned done, unsigned total)
{
    int progress = (int)(((uint64_t)stage * 1000u + (uint64_t)done * 1000u / total) / 6u);
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

static int benchmark_worker(void *unused)
{
    (void)unused;
    const papp_if_fs_t *fs = papp_fs();
    uint8_t *buffer = papp_alloc(SEQ_LARGE_BYTES, PAPP_MEM_LARGE, 64);
    uint32_t *order = papp_alloc(TEST_BLOCKS * sizeof(*order), PAPP_MEM_LARGE, 4);
    papp_file_t *file = NULL;
    bool have_file = false;
    int final_state = ST_ERROR;
    uint32_t verified = 0;

    if (!fs || !fs->open || !fs->close || !fs->read || !fs->write || !fs->seek ||
        !fs->remove || !fs->mkdir) {
        set_error("SD filesystem is unavailable");
        goto done;
    }
    if (!buffer || !order) {
        set_error("Not enough memory for test buffer");
        goto done;
    }

    /* The app owns this directory and file; never overwrite a user-selected path. */
    (void)fs->mkdir("/sd/data");
    (void)fs->mkdir("/sd/data/tool.sd_speedtest");
    (void)fs->remove(TEST_PATH);  /* clean a stale file left by a previous reset */

    for (unsigned i = 0; i < TEST_BLOCKS; i++) order[i] = i;
    uint32_t seed = 0x51DCA4D3u;
    for (unsigned i = TEST_BLOCKS - 1; i > 0; i--) {
        seed = seed * 1664525u + 1013904223u;
        unsigned j = seed % (i + 1u);
        uint32_t t = order[i]; order[i] = order[j]; order[j] = t;
    }

    /* SEQ 1 MiB, one synchronous request at a time (the PAPP FS API has no queued I/O). */
    fill_expected(buffer, 64u * 1024u, 0);
    for (size_t i = 64u * 1024u; i < SEQ_LARGE_BYTES; i += 64u * 1024u)
        memcpy(buffer + i, buffer, 64u * 1024u); /* the pattern repeats every 64 KiB */
    update_state(ST_WRITE, 0);
    file = fs->open(TEST_PATH, "wb");
    if (!file) { set_error("Could not create the temporary test file"); goto done; }
    have_file = true;
    int64_t started = papp_time_us();
    for (uint32_t offset = 0; offset < TEST_BYTES; offset += SEQ_LARGE_BYTES) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        if (fs->write(file, buffer, SEQ_LARGE_BYTES) != SEQ_LARGE_BYTES) {
            set_error("SD write failed or was incomplete");
            goto cleanup;
        }
        update_progress(ST_WRITE, 0, offset + SEQ_LARGE_BYTES, TEST_BYTES);
    }
    if (fs->close(file) != 0) {
        file = NULL;
        set_error("Could not finish writing the test file");
        goto cleanup;
    }
    file = NULL;
    int64_t elapsed = papp_time_us() - started;
    if (elapsed > 0) save_rate(0, true, (double)TEST_BYTES / (double)elapsed);

    file = fs->open(TEST_PATH, "rb");
    if (!file) {
        set_error("Could not reopen the test file for reading");
        goto cleanup;
    }
    update_state(ST_READ, 166);
    int64_t read_elapsed = 0;
    for (uint32_t offset = 0; offset < TEST_BYTES; offset += SEQ_LARGE_BYTES) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        int64_t read_started = papp_time_us();
        size_t got = fs->read(file, buffer, SEQ_LARGE_BYTES);
        read_elapsed += papp_time_us() - read_started;
        if (got != SEQ_LARGE_BYTES) {
            set_error("SD read failed or ended early");
            goto cleanup;
        }
        for (size_t i = 0; i < SEQ_LARGE_BYTES; i++) {
            if (buffer[i] != expected_byte(offset + (uint32_t)i)) {
                set_error("Readback check failed; card data differs");
                goto cleanup;
            }
        }
        verified += SEQ_LARGE_BYTES;
        update_progress(ST_READ, 1, offset + SEQ_LARGE_BYTES, TEST_BYTES);
    }
    if (fs->close(file) != 0) {
        file = NULL;
        set_error("Could not close the read test file");
        goto cleanup;
    }
    file = NULL;
    if (read_elapsed > 0) save_rate(0, false, (double)TEST_BYTES / (double)read_elapsed);

    /* SEQ 4 KiB, one synchronous request at a time. */
    file = fs->open(TEST_PATH, "wb");
    if (!file) { set_error("Could not reopen the test file for 4 KiB writes"); goto cleanup; }
    int64_t write_elapsed = 0;
    for (uint32_t offset = 0; offset < TEST_BYTES; offset += IO_4K_BYTES) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        fill_expected(buffer, IO_4K_BYTES, offset);
        int64_t t0 = papp_time_us();
        if (fs->write(file, buffer, IO_4K_BYTES) != IO_4K_BYTES) {
            set_error("4 KiB sequential write failed"); goto cleanup;
        }
        write_elapsed += papp_time_us() - t0;
        if ((offset & 0x1FFFFu) == 0) update_progress(ST_WRITE, 2, offset + IO_4K_BYTES, TEST_BYTES);
    }
    int64_t close_started = papp_time_us();
    if (fs->close(file) != 0) { file = NULL; set_error("Could not close the 4 KiB write test"); goto cleanup; }
    write_elapsed += papp_time_us() - close_started;
    file = NULL;
    if (write_elapsed > 0) save_rate(1, true, (double)TEST_BYTES / (double)write_elapsed);

    file = fs->open(TEST_PATH, "rb");
    if (!file) { set_error("Could not reopen the test file for 4 KiB reads"); goto cleanup; }
    read_elapsed = 0;
    for (uint32_t offset = 0; offset < TEST_BYTES; offset += IO_4K_BYTES) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        int64_t t0 = papp_time_us();
        size_t got = fs->read(file, buffer, IO_4K_BYTES);
        read_elapsed += papp_time_us() - t0;
        if (got != IO_4K_BYTES) { set_error("4 KiB sequential read failed"); goto cleanup; }
        for (size_t i = 0; i < IO_4K_BYTES; i++)
            if (buffer[i] != expected_byte(offset + (uint32_t)i)) { set_error("4 KiB readback differs"); goto cleanup; }
        if ((offset & 0x1FFFFu) == 0) update_progress(ST_READ, 3, offset + IO_4K_BYTES, TEST_BYTES);
    }
    if (fs->close(file) != 0) { file = NULL; set_error("Could not close the 4 KiB read test"); goto cleanup; }
    file = NULL;
    if (read_elapsed > 0) save_rate(1, false, (double)TEST_BYTES / (double)read_elapsed);

    /* RND 4 KiB, 256 scattered requests over the 8 MiB test file. */
    file = fs->open(TEST_PATH, "rb");
    if (!file) { set_error("Could not reopen the test file for random reads"); goto cleanup; }
    read_elapsed = 0;
    for (unsigned i = 0; i < RANDOM_OPS; i++) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        uint32_t offset = order[i] * IO_4K_BYTES;
        int64_t t0 = papp_time_us();
        int seek_error = fs->seek(file, (long)offset, SEEK_SET);
        size_t got = seek_error == 0 ? fs->read(file, buffer, IO_4K_BYTES) : 0;
        read_elapsed += papp_time_us() - t0;
        if (got != IO_4K_BYTES) { set_error("Random 4 KiB read failed"); goto cleanup; }
        for (size_t j = 0; j < IO_4K_BYTES; j++)
            if (buffer[j] != expected_byte(offset + (uint32_t)j)) { set_error("Random readback differs"); goto cleanup; }
        update_progress(ST_READ, 4, i + 1u, RANDOM_OPS);
    }
    if (fs->close(file) != 0) { file = NULL; set_error("Could not close random read test"); goto cleanup; }
    file = NULL;
    if (read_elapsed > 0) save_rate(2, false, (double)(RANDOM_OPS * IO_4K_BYTES) / (double)read_elapsed);

    file = fs->open(TEST_PATH, "r+b");
    if (!file) { set_error("Could not open the test file for random writes"); goto cleanup; }
    write_elapsed = 0;
    for (unsigned i = 0; i < RANDOM_OPS; i++) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        uint32_t offset = order[i] * IO_4K_BYTES;
        fill_expected(buffer, IO_4K_BYTES, offset);
        int64_t t0 = papp_time_us();
        if (fs->seek(file, (long)offset, SEEK_SET) != 0 || fs->write(file, buffer, IO_4K_BYTES) != IO_4K_BYTES) {
            set_error("Random 4 KiB write failed"); goto cleanup;
        }
        write_elapsed += papp_time_us() - t0;
        update_progress(ST_WRITE, 5, i + 1u, RANDOM_OPS);
    }
    close_started = papp_time_us();
    if (fs->close(file) != 0) { file = NULL; set_error("Could not close random write test"); goto cleanup; }
    write_elapsed += papp_time_us() - close_started;
    file = NULL;
    if (write_elapsed > 0) save_rate(2, true, (double)(RANDOM_OPS * IO_4K_BYTES) / (double)write_elapsed);

    /* Verify the full final file after the scattered writes. */
    file = fs->open(TEST_PATH, "rb");
    if (!file) { set_error("Could not reopen the test file for final verification"); goto cleanup; }
    for (uint32_t offset = 0; offset < TEST_BYTES; offset += SEQ_LARGE_BYTES) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        size_t got = fs->read(file, buffer, SEQ_LARGE_BYTES);
        if (got != SEQ_LARGE_BYTES) { set_error("Final SD readback was incomplete"); goto cleanup; }
        for (size_t i = 0; i < SEQ_LARGE_BYTES; i++)
            if (buffer[i] != expected_byte(offset + (uint32_t)i)) { set_error("Final SD verification failed"); goto cleanup; }
    }
    if (fs->close(file) != 0) { file = NULL; set_error("Could not close final verification file"); goto cleanup; }
    file = NULL;
    verified = TEST_BYTES;
    final_state = ST_DONE;

cleanup:
    if (file) { fs->close(file); file = NULL; }
    if (have_file && fs->remove(TEST_PATH) != 0 && final_state != ST_ERROR) {
        set_error("Cleanup failed; benchmark.tmp remains in the app data folder");
        final_state = ST_ERROR;
    }
    papp_mutex_lock(s_lock);
    if (final_state == ST_DONE) {
        s_result.verified_bytes = verified;
        s_result.error[0] = 0;
    } else if (final_state == ST_CANCELLED) {
        s_result.error[0] = 0;
    }
    s_result.state = final_state;
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
    switch (r->state) {
    case ST_WRITE: return "Writing SD benchmark data…";
    case ST_READ: return "Reading and verifying SD data…";
    case ST_DONE: return "Readback verified; temporary file removed";
    case ST_ERROR: return r->error[0] ? r->error : "Benchmark failed";
    case ST_CANCELLED: return "Cancelled; temporary file cleaned up";
    default: return "Ready • 8 MiB test file • sequential and random 4 KiB I/O";
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
    char value[48];
    static const char *const names[] = {
        "Sequential 1 MiB (Q1T1)",
        "Sequential 4 KiB (Q1T1)",
        "Random 4 KiB (256 requests)",
    };
    ui_begin(s_surface);
    ui_bar("SD Card Speed Test", NULL, 0, 0, state_label(&r),
           r.state == ST_ERROR ? C_RED : r.state == ST_DONE ? C_GREEN : C_BLUE, false);
    ui_text_c(ui_small, 400, 112, sd_bus_label(), C_DIM);
    ui_text_l(ui_small, 60, 145, "Test profile", C_FAINT);
    ui_text_c(ui_small, 482, 145, "Read (MB/s)", C_DIM);
    ui_text_c(ui_small, 684, 145, "Write (MB/s)", C_DIM);
    for (unsigned i = 0; i < 3; i++) {
        int y = 164 + (int)i * 52;
        ui_rrect(40, y, 720, 44, 8, C_PANEL);
        ui_text_l(ui_small, 58, y + 22, names[i], C_TEXT);
        if (r.read_mb_s[i] > 0.0) snprintf(value, sizeof(value), "%.2f", r.read_mb_s[i]);
        else snprintf(value, sizeof(value), "—");
        ui_text_c(ui_small, 482, y + 22, value, C_GREEN);
        if (r.write_mb_s[i] > 0.0) snprintf(value, sizeof(value), "%.2f", r.write_mb_s[i]);
        else snprintf(value, sizeof(value), "—");
        ui_text_c(ui_small, 684, y + 22, value, C_OUT);
    }

    ui_rrect(48, 350, 632, 18, 9, C_RAISED);
    int filled = (r.progress * 632) / 1000;
    if (filled > 0) ui_rrect(48, 350, filled, 18, 9, C_BLUE);
    snprintf(value, sizeof(value), "%d%%", r.progress / 10);
    ui_text_r(ui_small, 752, 359, value, C_TEXT);

    ui_button(292, 396, 216, 50, is_running(r.state) ? "Cancel" : "Run Test",
              is_running(r.state) ? C_RED : C_BLUE, ACT_RUN);
    ui_text_c(ui_small, 400, 460, "A / Touch: Start   •   B: Cancel   •   X: Exit", C_DIM);
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

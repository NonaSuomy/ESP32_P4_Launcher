/*
 * SD Card Speed Test: sequential write/read benchmark for the mounted SD card.
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
/* The generated pattern repeats every 64 KiB, so one prefilled chunk can be reused. */
#define CHUNK_BYTES (64u * 1024u)
#define TEST_MIB (1024.0 * 1024.0)

enum { ST_IDLE, ST_WRITE, ST_READ, ST_DONE, ST_ERROR, ST_CANCELLED };
enum { ACT_RUN = 1 };

typedef struct {
    int state;
    int progress;                 /* 0..1000 */
    double write_mib_s;
    double read_mib_s;
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

static bool cancelled(void)
{
    return s_cancel || papp_exit_requested();
}

static int benchmark_worker(void *unused)
{
    (void)unused;
    const papp_if_fs_t *fs = papp_fs();
    uint8_t *buffer = papp_alloc(CHUNK_BYTES, PAPP_MEM_LARGE, 64);
    papp_file_t *file = NULL;
    bool have_file = false;
    int final_state = ST_ERROR;
    double write_rate = 0.0, read_rate = 0.0;
    uint32_t verified = 0;

    if (!fs || !fs->open || !fs->close || !fs->read || !fs->write || !fs->remove || !fs->mkdir) {
        set_error("SD filesystem is unavailable");
        goto done;
    }
    if (!buffer) {
        set_error("Not enough memory for test buffer");
        goto done;
    }

    /* The app owns this directory and file; never overwrite a user-selected path. */
    (void)fs->mkdir("/sd/data");
    (void)fs->mkdir("/sd/data/tool.sd_speedtest");
    (void)fs->remove(TEST_PATH);  /* clean a stale file left by a previous reset */
    file = fs->open(TEST_PATH, "wb");
    if (!file) {
        set_error("Could not create the temporary test file");
        goto done;
    }
    have_file = true;

    for (size_t i = 0; i < CHUNK_BYTES; i++) buffer[i] = expected_byte((uint32_t)i);
    update_state(ST_WRITE, 0);
    int64_t started = papp_time_us();
    for (uint32_t offset = 0; offset < TEST_BYTES; offset += CHUNK_BYTES) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        size_t count = TEST_BYTES - offset;
        if (count > CHUNK_BYTES) count = CHUNK_BYTES;
        if (fs->write(file, buffer, count) != count) {
            set_error("SD write failed or was incomplete");
            goto cleanup;
        }
        update_state(ST_WRITE, (int)(((uint64_t)(offset + count) * 500u) / TEST_BYTES));
    }
    if (fs->close(file) != 0) {
        file = NULL;
        set_error("Could not finish writing the test file");
        goto cleanup;
    }
    file = NULL;
    int64_t elapsed = papp_time_us() - started;
    if (elapsed > 0) write_rate = (double)TEST_BYTES * 1000000.0 / ((double)elapsed * TEST_MIB);

    file = fs->open(TEST_PATH, "rb");
    if (!file) {
        set_error("Could not reopen the test file for reading");
        goto cleanup;
    }
    update_state(ST_READ, 500);
    int64_t read_elapsed = 0;
    for (uint32_t offset = 0; offset < TEST_BYTES; offset += CHUNK_BYTES) {
        if (cancelled()) { final_state = ST_CANCELLED; goto cleanup; }
        size_t count = TEST_BYTES - offset;
        if (count > CHUNK_BYTES) count = CHUNK_BYTES;
        int64_t read_started = papp_time_us();
        size_t got = fs->read(file, buffer, count);
        read_elapsed += papp_time_us() - read_started;
        if (got != count) {
            set_error("SD read failed or ended early");
            goto cleanup;
        }
        for (size_t i = 0; i < count; i++) {
            if (buffer[i] != expected_byte(offset + (uint32_t)i)) {
                set_error("Readback check failed; card data differs");
                goto cleanup;
            }
        }
        verified += (uint32_t)count;
        update_state(ST_READ, 500 + (int)(((uint64_t)(offset + count) * 500u) / TEST_BYTES));
    }
    if (fs->close(file) != 0) {
        file = NULL;
        set_error("Could not close the read test file");
        goto cleanup;
    }
    file = NULL;
    if (read_elapsed > 0) read_rate = (double)TEST_BYTES * 1000000.0 / ((double)read_elapsed * TEST_MIB);
    final_state = ST_DONE;

cleanup:
    if (file) { fs->close(file); file = NULL; }
    if (have_file && fs->remove(TEST_PATH) != 0 && final_state != ST_ERROR) {
        set_error("Cleanup failed; benchmark.tmp remains in the app data folder");
        final_state = ST_ERROR;
    }
    papp_mutex_lock(s_lock);
    if (final_state == ST_DONE) {
        s_result.write_mib_s = write_rate;
        s_result.read_mib_s = read_rate;
        s_result.verified_bytes = verified;
        s_result.error[0] = 0;
    } else if (final_state == ST_CANCELLED) {
        s_result.error[0] = 0;
    }
    s_result.state = final_state;
    papp_mutex_unlock(s_lock);

done:
    if (buffer) papp_free(buffer);
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
    case ST_WRITE: return "Writing test data…";
    case ST_READ: return "Reading and verifying…";
    case ST_DONE: return "Readback verified; temporary file removed";
    case ST_ERROR: return r->error[0] ? r->error : "Benchmark failed";
    case ST_CANCELLED: return "Cancelled; temporary file cleaned up";
    default: return "Ready • 8 MiB temporary file • write, read, verify, remove";
    }
}

static void draw_screen(void)
{
    screen_state_t r = snapshot();
    char value[48];
    ui_begin(s_surface);
    ui_bar("SD Card Speed Test", NULL, 0, 0, state_label(&r),
           r.state == ST_ERROR ? C_RED : r.state == ST_DONE ? C_GREEN : C_BLUE, false);

    ui_rrect(32, 128, 354, 204, 18, C_PANEL);
    ui_rrect(414, 128, 354, 204, 18, C_PANEL);
    ui_text_c(ui_med, 209, 164, "Sequential write", C_DIM);
    ui_text_c(ui_med, 591, 164, "Sequential read", C_DIM);
    if (r.state == ST_DONE) {
        snprintf(value, sizeof(value), "%.2f MiB/s", r.write_mib_s);
        ui_text_c(ui_large, 209, 224, value, C_OUT);
        snprintf(value, sizeof(value), "%.2f MiB/s", r.read_mib_s);
        ui_text_c(ui_large, 591, 224, value, C_GREEN);
        snprintf(value, sizeof(value), "%u MiB verified", (unsigned)(r.verified_bytes / (1024u * 1024u)));
        ui_text_c(ui_small, 400, 286, value, C_DIM);
    } else {
        ui_text_c(ui_large, 209, 224, "—", C_OUT);
        ui_text_c(ui_large, 591, 224, "—", C_GREEN);
        ui_text_c(ui_small, 400, 286, "Results appear here when the test completes", C_DIM);
    }

    ui_rrect(48, 350, 704, 18, 9, C_RAISED);
    int filled = (r.progress * 704) / 1000;
    if (filled > 0) ui_rrect(48, 350, filled, 18, 9, C_BLUE);
    snprintf(value, sizeof(value), "%d%%", r.progress / 10);
    ui_text_c(ui_small, 400, 389, value, C_TEXT);

    ui_button(292, 414, 216, 50, is_running(r.state) ? "Cancel" : "Run Test",
              is_running(r.state) ? C_RED : C_BLUE, ACT_RUN);
    ui_text_c(ui_small, 400, 402, "A / touch: start   •   B: cancel   •   X: exit", C_DIM);
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

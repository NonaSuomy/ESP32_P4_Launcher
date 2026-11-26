/*
 * irq_test — IRQ interface test (build with --libc). Needs no wiring: a pin set up as an output sees its
 * own edges, so the test toggles a free header pin and takes the interrupts.
 *   args: [mode] [pin]
 *     all    (default) every check below; exit code = failed checks
 *     leave  attaches a pin and two timers and returns: the launcher must free them
 *   pin: the GPIO to toggle (default: the lowest free pin). Nothing may be connected to it.
 *
 * Checks: free-pin mask and refusals; deferred pin interrupts on core 0 and core 1 (count, latency);
 * direct pin handler (runs on the chosen core); level trigger fires once until enable(); periodic timer
 * (deferred) count and jitter; direct timer at 50 us on core 1 while code_alloc turns the caches off;
 * one-shot timer re-armed from its own handler; signal() from a direct handler; papp_irq_thread; detach.
 */
#include "papp/papp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "test.irq", .name = "IRQ test", .version = PAPP_APP_VERSION, .main_stack = 16384,
              .flags = PAPP_MF_HIDDEN);

static int s_fail;
static const papp_if_irq_t *irq;

#define CHECK(cond, ...) do {                                                   \
        if (cond) printf("ok   " __VA_ARGS__);                                  \
        else { printf("FAIL " __VA_ARGS__); s_fail++; }                         \
    } while (0)

static inline int hart(void)
{
    int h;
    __asm__ volatile("csrr %0, mhartid" : "=r"(h));
    return h;
}

/* ── Direct handlers: FAST code, FAST data, no calls but ctx's ─────────── */

PAPP_FAST_BSS static volatile uint32_t d_count;
PAPP_FAST_BSS static volatile int      d_core;
PAPP_FAST_BSS static volatile uint32_t d_arm_left;
PAPP_FAST_BSS static volatile int64_t  d_last, d_max_gap, d_min_gap;

PAPP_FAST static void direct_count(void *arg, const papp_irq_ctx_t *ctx)
{
    (void)arg;
    d_count++;
    d_core = hart();
    int64_t t = ctx->ev->time_us;
    if (d_last) {
        int64_t g = t - d_last;
        if (g > d_max_gap) d_max_gap = g;
        if (g < d_min_gap) d_min_gap = g;
    }
    d_last = t;
}

PAPP_FAST static void direct_rearm(void *arg, const papp_irq_ctx_t *ctx)
{
    (void)arg;
    d_count++;
    d_core = hart();
    if (d_arm_left) {
        d_arm_left--;
        ctx->timer_arm(ctx->self, 200);
    }
}

PAPP_FAST static void direct_signal_every_10(void *arg, const papp_irq_ctx_t *ctx)
{
    d_count++;
    if (ctx->ev->count % 10 == 0) ctx->signal((papp_irq_t *)arg, ctx->ev);
}

PAPP_FAST_BSS static volatile int64_t d_t0, d_lat_sum, d_lat_max;

PAPP_FAST static void direct_lat(void *arg, const papp_irq_ctx_t *ctx)
{
    (void)arg;
    d_count++;
    d_core = hart();
    int64_t l = ctx->ev->time_us - d_t0;
    d_lat_sum += l;
    if (l > d_lat_max) d_lat_max = l;
}

static void not_fast(void *arg, const papp_irq_ctx_t *ctx) { (void)arg; (void)ctx; }

static void direct_reset(void)
{
    d_count = 0; d_core = -1; d_last = 0; d_max_gap = 0; d_min_gap = INT64_MAX;
}

static void spin_us(int us)
{
    int64_t end = papp_time_us() + us;
    while (papp_time_us() < end) { }
}

/* ── Checks ───────────────────────────────────────────────────────────── */

static void test_refusals(int pin)
{
    uint64_t mask = irq->gpio_mask();
    int bad = -1;
    for (int p = 0; p < 57; p++) if (!((mask >> p) & 1)) { bad = p; break; }
    CHECK(irq->gpio_config(bad, PAPP_GPIO_INPUT, 0) == PAPP_E_ARG, "a board pin (GPIO %d) is refused\n", bad);
    CHECK(irq->gpio_write(pin, 1) == PAPP_E_ARG, "write before gpio_config is refused\n");
    papp_irq_t *h = NULL;
    CHECK(irq->gpio_attach(pin, PAPP_IRQ_RISING, NULL, &h) == PAPP_E_ARG, "attach before gpio_config is refused\n");
    papp_irq_cfg_t c = { .core = 1, .isr = not_fast };
    CHECK(irq->timer_attach(&c, &h) == PAPP_E_UNSUPPORTED, "a direct handler outside FAST is refused\n");
    c = (papp_irq_cfg_t){ .core = 2 };
    CHECK(irq->timer_attach(&c, &h) == PAPP_E_ARG, "core 2 is refused\n");
}

static void test_pin_deferred(int pin, int core)
{
    irq->gpio_config(pin, PAPP_GPIO_OUTPUT, PAPP_GPIO_PULL_NONE);
    irq->gpio_write(pin, 0);
    papp_irq_cfg_t c = { .core = (int8_t)core, .queue = 64 };
    papp_irq_t *h = NULL;
    int r = irq->gpio_attach(pin, PAPP_IRQ_ANY_EDGE, &c, &h);
    CHECK(r == PAPP_E_OK, "pin %d deferred on core %d: attach (%d)\n", pin, core, r);
    if (r) return;
    CHECK(irq->core(h) == core, "core() says %d\n", irq->core(h));
    papp_irq_t *h2 = NULL;
    CHECK(irq->gpio_attach(pin, PAPP_IRQ_RISING, &c, &h2) == PAPP_E_LIMIT, "a second attach on the pin is refused\n");

    const int N = 200;
    int got = 0, lost = 0, order_ok = 1;
    int64_t lat_sum = 0, lat_max = 0, wake_max = 0;
    for (int i = 0; i < N; i++) {
        int64_t t0 = papp_time_us();
        irq->gpio_write(pin, (i & 1) ? 0 : 1);
        papp_irq_event_t ev;
        if (irq->wait(h, &ev, 100) == 1) {
            int64_t now = papp_time_us();
            got++;
            lost += ev.dropped;
            if (ev.level != ((i & 1) ? 0 : 1) || ev.pin != pin) order_ok = 0;
            int64_t lat = ev.time_us - t0;
            lat_sum += lat;
            if (lat > lat_max) lat_max = lat;
            if (now - ev.time_us > wake_max) wake_max = now - ev.time_us;
        }
    }
    CHECK(got == N && lost == 0, "%d of %d edges taken, %d lost\n", got, N, lost);
    CHECK(order_ok, "each event has its pin and the new level\n");
    printf("     write -> interrupt: avg %lld us, max %lld us; interrupt -> waiting thread max %lld us\n",
           got ? (long long)(lat_sum / got) : 0, (long long)lat_max, (long long)wake_max);

    /* a burst faster than anyone takes it: the queue keeps 64, the rest is counted */
    for (int i = 0; i < 200; i++) { irq->gpio_write(pin, i & 1 ? 0 : 1); spin_us(5); }
    int n = 0, dropped = 0;
    papp_irq_event_t ev;
    while (irq->wait(h, &ev, 20) == 1) { n++; dropped += ev.dropped; }
    printf("     burst of 200 edges at 5 us: %d queued, %d dropped (counted)\n", n, dropped);
    CHECK(n > 0 && n <= 64, "the queue holds at most its 64\n");

    irq->enable(h, false);
    irq->gpio_write(pin, 1); irq->gpio_write(pin, 0);
    CHECK(irq->wait(h, &ev, 20) == PAPP_E_TIMEOUT, "no events while disabled\n");
    irq->detach(h);
    CHECK(irq->wait(h, &ev, 0) == PAPP_E_ARG, "a detached handle is refused\n");
}

static void test_pin_direct(int pin, int core)
{
    irq->gpio_config(pin, PAPP_GPIO_OUTPUT, PAPP_GPIO_PULL_NONE);
    irq->gpio_write(pin, 0);
    direct_reset();
    papp_irq_cfg_t c = { .core = (int8_t)core, .priority = 3, .isr = direct_count };
    papp_irq_t *h = NULL;
    int r = irq->gpio_attach(pin, PAPP_IRQ_RISING, &c, &h);
    CHECK(r == PAPP_E_OK, "pin %d direct on core %d: attach (%d)\n", pin, core, r);
    if (r) return;
    for (int i = 0; i < 1000; i++) { irq->gpio_write(pin, 1); spin_us(3); irq->gpio_write(pin, 0); spin_us(3); }
    papp_delay_ms(5);
    printf("     1000 rising edges 6 us apart: the handler ran %lu times (edges closer than its latency merge)\n",
           (unsigned long)d_count);
    CHECK(d_core == core, "the handler ran on core %d\n", d_core);
    irq->detach(h);

    direct_reset();
    d_lat_sum = d_lat_max = 0;
    c.isr = direct_lat;
    irq->gpio_write(pin, 0);
    if (irq->gpio_attach(pin, PAPP_IRQ_RISING, &c, &h) != PAPP_E_OK) { CHECK(0, "latency attach\n"); return; }
    for (int i = 0; i < 1000; i++) {
        d_t0 = papp_time_us();
        irq->gpio_write(pin, 1);
        spin_us(50);
        irq->gpio_write(pin, 0);
        spin_us(50);
    }
    papp_delay_ms(5);
    CHECK(d_count == 1000, "1000 rising edges 100 us apart: %lu runs\n", (unsigned long)d_count);
    printf("     write -> direct handler on core %d: avg %lld us, max %lld us\n", core,
           d_count ? (long long)(d_lat_sum / (int64_t)d_count) : 0, (long long)d_lat_max);

    irq->detach(h);

    /* the same with PAPP_IRQ_AWAKE: the core doesn't sleep while it is attached */
    direct_reset();
    d_lat_sum = d_lat_max = 0;
    c.flags = PAPP_IRQ_AWAKE;
    if (irq->gpio_attach(pin, PAPP_IRQ_RISING, &c, &h) != PAPP_E_OK) { CHECK(0, "AWAKE attach\n"); return; }
    for (int i = 0; i < 1000; i++) {
        d_t0 = papp_time_us();
        irq->gpio_write(pin, 1);
        spin_us(50);
        irq->gpio_write(pin, 0);
        spin_us(50);
    }
    papp_delay_ms(5);
    int64_t avg = d_count ? d_lat_sum / (int64_t)d_count : 0;
    CHECK(d_count == 1000 && avg <= 6, "AWAKE on core %d: %lu runs, avg %lld us, max %lld us\n", core,
          (unsigned long)d_count, (long long)avg, (long long)d_lat_max);
    irq->detach(h);
}

static void test_level(int pin)
{
    irq->gpio_config(pin, PAPP_GPIO_OUTPUT, PAPP_GPIO_PULL_NONE);
    irq->gpio_write(pin, 1);
    papp_irq_cfg_t c = { .core = 0 };
    papp_irq_t *h = NULL;
    if (irq->gpio_attach(pin, PAPP_IRQ_HIGH, &c, &h) != PAPP_E_OK) { CHECK(0, "level attach\n"); return; }
    papp_irq_event_t ev;
    int n = 0;
    while (irq->wait(h, &ev, 30) == 1) n++;
    CHECK(n == 1, "a high level fires once (%d)\n", n);
    irq->enable(h, true);
    n = 0;
    while (irq->wait(h, &ev, 30) == 1) n++;
    CHECK(n == 1, "and once more after enable() (%d)\n", n);
    irq->gpio_write(pin, 0);
    irq->enable(h, true);
    CHECK(irq->wait(h, &ev, 30) == PAPP_E_TIMEOUT, "not while the level is low\n");
    irq->gpio_write(pin, 1);
    CHECK(irq->wait(h, &ev, 30) == 1 && ev.level == 1, "and again when it goes high\n");
    irq->detach(h);
    irq->gpio_write(pin, 0);
}

static void test_timer_deferred(int flags)
{
    papp_irq_cfg_t c = { .core = 1, .queue = 64, .flags = (uint8_t)flags };
    papp_irq_t *t = NULL;
    int r = irq->timer_attach(&c, &t);
    CHECK(r == PAPP_E_OK, "timer deferred on core 1%s: attach (%d)\n", flags ? " (AWAKE)" : "", r);
    if (r) return;
    irq->timer_start(t, 1000, true);
    int n = 0;
    int64_t prev = 0, gmin = INT64_MAX, gmax = 0, end = papp_time_us() + 1000000;
    papp_irq_event_t ev;
    while (papp_time_us() < end && irq->wait(t, &ev, 50) == 1) {
        if (prev) { int64_t g = ev.time_us - prev; if (g < gmin) gmin = g; if (g > gmax) gmax = g; }
        prev = ev.time_us;
        n++;
    }
    irq->timer_stop(t);
    CHECK(n >= 990 && n <= 1010, "1 ms periodic for 1 s: %d events\n", n);
    int tol = flags ? 75 : 150;   /* core 1 idle: interrupts start late (papp_abi.h PAPP_IRQ_AWAKE) */
    CHECK(gmin >= 1000 - tol && gmax <= 1000 + tol, "interval %lld..%lld us (within %d)\n",
          (long long)gmin, (long long)gmax, tol);
    while (irq->wait(t, &ev, 10) == 1) { }
    CHECK(irq->wait(t, &ev, 20) == PAPP_E_TIMEOUT, "nothing after timer_stop\n");
    irq->detach(t);
}

static void test_timer_direct_cache_off(void)
{
    direct_reset();
    papp_irq_cfg_t c = { .core = 1, .priority = 3, .isr = direct_count };
    papp_irq_t *t = NULL;
    int r = irq->timer_attach(&c, &t);
    CHECK(r == PAPP_E_OK, "timer direct on core 1: attach (%d)\n", r);
    if (r) return;
    irq->timer_start(t, 50, true);
    int64_t t0 = papp_time_us();
    int maps = 0;
    while (papp_time_us() - t0 < 1000000) {        /* each one maps and unmaps: caches off twice */
        void *code = papp_code_alloc(4096);
        if (code) { papp_code_free(code); maps++; }
        papp_delay_ms(2);
    }
    irq->timer_stop(t);
    uint32_t n = d_count;
    CHECK(n >= 19500 && n <= 20200, "50 us for 1 s with %d code_alloc/free: %lu interrupts\n", maps, (unsigned long)n);
    CHECK(d_core == 1, "on core %d\n", d_core);
    CHECK(d_max_gap < 150, "longest gap %lld us, shortest %lld us\n", (long long)d_max_gap, (long long)d_min_gap);
    irq->detach(t);
}

static void test_timer_rearm(void)
{
    direct_reset();
    d_arm_left = 999;
    papp_irq_cfg_t c = { .core = 0, .isr = direct_rearm };
    papp_irq_t *t = NULL;
    if (irq->timer_attach(&c, &t) != PAPP_E_OK) { CHECK(0, "one-shot attach\n"); return; }
    int64_t t0 = papp_time_us();
    irq->timer_start(t, 200, false);
    while (d_count < 1000 && papp_time_us() - t0 < 1000000) papp_delay_ms(5);
    int64_t el = papp_time_us() - t0;
    CHECK(d_count == 1000, "one-shot re-armed from its handler: %lu runs in %lld ms\n",
          (unsigned long)d_count, (long long)(el / 1000));
    CHECK(d_core == 0, "on core %d\n", d_core);
    irq->detach(t);
}

static void test_signal(void)
{
    direct_reset();
    papp_irq_cfg_t cq = { .core = 1, .queue = 64 };
    papp_irq_t *q = NULL, *t = NULL;
    if (irq->timer_attach(&cq, &q) != PAPP_E_OK) { CHECK(0, "signal: target\n"); return; }
    papp_irq_cfg_t c = { .core = 1, .isr = direct_signal_every_10, .isr_arg = q };
    if (irq->timer_attach(&c, &t) != PAPP_E_OK) {
        CHECK(0, "signal: source\n");
        irq->detach(q);
        return;
    }
    irq->timer_start(t, 100, true);
    int n = 0;
    papp_irq_event_t ev;
    int64_t end = papp_time_us() + 500000;
    while (papp_time_us() < end) if (irq->wait(q, &ev, 50) == 1) { n++; if (ev.count % 10) n = -100000; }
    irq->timer_stop(t);
    CHECK(n >= 480 && n <= 520, "signal() every 10th of 100 us: %d events on the other handle\n", n);
    irq->detach(t);
    irq->detach(q);
}

static volatile int s_thr_events;
static void on_edge(void *arg, const papp_irq_event_t *ev) { (void)arg; (void)ev; s_thr_events++; }

static void test_thread(int pin)
{
    irq->gpio_config(pin, PAPP_GPIO_OUTPUT, PAPP_GPIO_PULL_NONE);
    irq->gpio_write(pin, 0);
    papp_irq_cfg_t c = { .core = 1, .queue = 32 };
    papp_irq_t *h = NULL;
    if (irq->gpio_attach(pin, PAPP_IRQ_FALLING, &c, &h) != PAPP_E_OK) { CHECK(0, "thread attach\n"); return; }
    s_thr_events = 0;
    papp_thread_t *th = papp_irq_thread(h, on_edge, NULL, PAPP_PRIO_HIGH);
    CHECK(th != NULL, "papp_irq_thread started\n");
    for (int i = 0; i < 100; i++) { irq->gpio_write(pin, 1); papp_delay_ms(1); irq->gpio_write(pin, 0); papp_delay_ms(1); }
    papp_delay_ms(20);
    CHECK(s_thr_events == 100, "the thread's function ran for %d of 100 falling edges\n", s_thr_events);
    irq->detach(h);                                 /* its wait() answers E_EXIT: the thread ends */
    int code = -1;
    CHECK(th && papp_thread_join(th, 1000, &code), "the thread ended after detach\n");
}

static void test_limits(void)
{
    papp_irq_t *t[3] = { 0 };
    papp_irq_cfg_t c = { .core = 0 };
    int r0 = irq->timer_attach(&c, &t[0]), r1 = irq->timer_attach(&c, &t[1]), r2 = irq->timer_attach(&c, &t[2]);
    CHECK(r0 == 0 && r1 == 0 && r2 == PAPP_E_LIMIT, "two timers per app (%d %d %d)\n", r0, r1, r2);
    for (int i = 0; i < 3; i++) if (t[i]) irq->detach(t[i]);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";
    irq = papp_irq();
    if (!irq) { printf("FAIL no IRQ interface (launcher too old)\n"); return 1; }
    uint64_t mask = irq->gpio_mask();
    int pin = -1;
    if (argc > 2) pin = atoi(argv[2]);
    else for (int p = 0; p < 57; p++) if ((mask >> p) & 1) { pin = p; break; }
    printf("board %s, free pins 0x%016llx, testing on GPIO %d\n", papp_board_id(), (unsigned long long)mask, pin);

    if (!strcmp(mode, "hang")) {                    /* forced stop with everything running */
        papp_irq_t *h, *t1, *t2;
        irq->gpio_config(pin, PAPP_GPIO_OUTPUT, PAPP_GPIO_PULL_NONE);
        papp_irq_cfg_t c1 = { .core = 1, .isr = direct_count, .flags = PAPP_IRQ_AWAKE }, c0 = { .core = 0 };
        irq->gpio_attach(pin, PAPP_IRQ_ANY_EDGE, &c0, &h);
        papp_irq_thread(h, on_edge, NULL, PAPP_PRIO_HIGH);
        irq->timer_attach(&c1, &t1);
        irq->timer_attach(&c0, &t2);
        irq->timer_start(t1, 50, true);
        papp_irq_thread(t2, on_edge, NULL, PAPP_PRIO_NORMAL);
        irq->timer_start(t2, 500, true);
        printf("hanging with a pin, two timers and two waiting threads (stop me)\n");
        for (int i = 0;; i++) { irq->gpio_write(pin, i & 1); spin_us(20); }
    }
    if (!strcmp(mode, "leave")) {
        papp_irq_t *h, *t1, *t2;
        irq->gpio_config(pin, PAPP_GPIO_OUTPUT, PAPP_GPIO_PULL_UP);
        papp_irq_cfg_t c1 = { .core = 1, .isr = direct_count }, c0 = { .core = 0 };
        irq->gpio_attach(pin, PAPP_IRQ_ANY_EDGE, &c1, &h);
        irq->timer_attach(&c1, &t1);
        irq->timer_attach(&c0, &t2);
        irq->timer_start(t1, 100, true);
        irq->timer_start(t2, 1000, true);
        papp_delay_ms(200);
        printf("leaving 1 pin and 2 timers attached (%lu direct runs)\n", (unsigned long)d_count);
        return 0;
    }

    test_timer_deferred(0);
    test_timer_deferred(PAPP_IRQ_AWAKE);
    test_timer_direct_cache_off();
    test_timer_rearm();
    test_signal();
    test_limits();
    if (pin < 0) {
        printf("no free pins on this board: pin checks skipped\n");
    } else {
        test_refusals(pin);
        test_pin_deferred(pin, 0);
        test_pin_deferred(pin, 1);
        test_pin_direct(pin, 0);
        test_pin_direct(pin, 1);
        test_level(pin);
        test_thread(pin);
    }
    printf("irq_test: %s (%d failures)\n", s_fail ? "FAIL" : "PASS", s_fail);
    return s_fail;
}

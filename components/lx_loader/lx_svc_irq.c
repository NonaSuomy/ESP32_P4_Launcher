/*
 * IRQ: GPIO pins and hardware timers for apps, with their interrupts on the core the app picks (papp_abi.h).
 *
 * Pins. IDF's GPIO ISR service uses the GPIO block's interrupt line 0 (esp_hosted's SPI handshake pins
 * use it); the P4 has four, each with its own per-pin enable bit and status register. Apps get line 1 on
 * core 0 and line 2 on core 1, each with a dispatcher of ours in IRAM, so their pins never meet IDF's.
 * Timers: gptimer with CONFIG_GPTIMER_ISR_CACHE_SAFE / CTRL_FUNC_IN_IRAM. An interrupt is allocated on
 * the core of the task that allocates it, so every allocation and free runs on a short-lived task pinned
 * to that core; it runs only while that core is in no interrupt, so a pin or timer removed there can
 * never be in the middle of its handler.
 *
 * Everything the interrupt touches is internal RAM (handles, queues, tables) and IRAM code, so it keeps
 * working while the caches are off. An app's direct handler must be in its FAST region in SRAM.
 * Queue operations from interrupts on both cores (signal() can push to any handle) take s_spin.
 */
#include <string.h>
#include "lx_services.h"
#include "bsp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/interrupts.h"

static const char *TAG = "lx_irq";

#define NPINS       SOC_GPIO_PIN_COUNT
#define MAX_HANDLES (PAPP_IRQ_MAX_PINS + PAPP_IRQ_MAX_TIMERS)
#define MAX_QUEUE   64
#define MAGIC       0x51524931u          /* "1IRQ" */
#define KIND_PIN    1
#define KIND_TIMER  2

struct papp_irq {
    uint32_t          magic;
    uint8_t           kind, pin, core, trigger;
    volatile bool     enabled, dead, running;
    papp_irq_isr_t    isr;
    void             *isr_arg;
    gptimer_handle_t  timer;
    uint8_t           prio, flags;
    volatile uint32_t count;
    uint16_t          dropped;
    uint16_t          qlen, qhead, qn;
    volatile int      waiters;
    StaticSemaphore_t sem_buf;
    SemaphoreHandle_t sem;
    papp_irq_event_t  q[];
};

__attribute__((weak)) uint64_t bsp_app_gpio_mask(void) { return 0; }

static SemaphoreHandle_t s_lock;                 /* attach / detach / config (task side)       */
static portMUX_TYPE      s_spin = portMUX_INITIALIZER_UNLOCKED;
static struct papp_irq  *s_h[MAX_HANDLES];       /* the app's handles (validation)              */
static struct papp_irq  *s_pin_h[NPINS];
static uint32_t          s_core_lo[2], s_core_hi[2];   /* pins whose interrupt runs on core n   */
static intr_handle_t     s_gintr[2];             /* the dispatcher on core n                    */
static uint64_t          s_mask;                 /* pins free for this run (bsp_app_gpio_mask)  */
static uint64_t          s_cfg, s_out;           /* configured by the app / outputs             */
static volatile int      s_awake[2];
static TaskHandle_t      s_awake_task[2];             /* PAPP_IRQ_AWAKE handles on core n            */

static const uint8_t s_ena_bit[2] = { GPIO_LL_INTR1_ENA, GPIO_LL_INTR2_ENA };
static const int     s_source[2]  = { ETS_GPIO_INTR1_SOURCE, ETS_GPIO_INTR2_SOURCE };

static inline bool pin_in(uint64_t m, int pin) { return pin >= 0 && pin < NPINS && (m >> pin) & 1; }

/* ── Interrupt side (IRAM) ───────────────────────────────────────────────── */

static bool IRAM_ATTR valid_isr(const struct papp_irq *h)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (s_h[i] == h) return !h->dead;
    return false;
}

static void IRAM_ATTR push(struct papp_irq *h, const papp_irq_event_t *ev)
{
    bool ok = false;
    portENTER_CRITICAL_ISR(&s_spin);
    if (valid_isr(h)) {
        if (h->qn < h->qlen) {
            papp_irq_event_t *e = &h->q[(h->qhead + h->qn) % h->qlen];
            *e = *ev;
            e->dropped = h->dropped;
            h->dropped = 0;
            h->qn++;
            ok = true;
        } else if (h->dropped < 0xFFFF) {
            h->dropped++;
        }
    }
    portEXIT_CRITICAL_ISR(&s_spin);
    if (ok) {
        BaseType_t woke = pdFALSE;
        xSemaphoreGiveFromISR(h->sem, &woke);
        if (woke) portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR isr_gpio_write(int pin, int level)
{
    if (pin_in(s_out, pin)) gpio_ll_set_level(&GPIO, pin, level ? 1 : 0);
}

static int IRAM_ATTR isr_gpio_read(int pin)
{
    return pin_in(s_cfg, pin) ? gpio_ll_get_level(&GPIO, pin) : PAPP_E_ARG;
}

static void IRAM_ATTR isr_signal(papp_irq_t *h, const papp_irq_event_t *ev)
{
    if (h && ev) push(h, ev);
}

static void IRAM_ATTR isr_timer_arm(papp_irq_t *t, uint32_t us)
{
    if (!t || t->kind != KIND_TIMER || t->dead || !us) return;
    uint64_t now = 0;
    gptimer_get_raw_count(t->timer, &now);
    gptimer_alarm_config_t a = { .alarm_count = now + us };
    gptimer_set_alarm_action(t->timer, &a);
    if (!t->running && gptimer_start(t->timer) == ESP_OK) t->running = true;
}

static void IRAM_ATTR fire(struct papp_irq *h, uint8_t pin, uint8_t level)
{
    papp_irq_event_t ev = { .time_us = esp_timer_get_time(), .count = ++h->count, .pin = pin, .level = level };
    if (h->isr) {
        papp_irq_ctx_t c = {
            .ev = &ev, .self = h, .gpio_write = isr_gpio_write, .gpio_read = isr_gpio_read,
            .signal = isr_signal,
            .timer_arm = isr_timer_arm,
        };
        h->isr(h->isr_arg, &c);
    } else {
        push(h, &ev);
    }
}

static void IRAM_ATTR gpio_dispatch(void *arg)
{
    int core = (int)(intptr_t)arg;
    uint32_t lo = core ? GPIO.intr_2.int_2 : GPIO.intr_1.int_1;
    uint32_t hi = core ? GPIO.intr1_2.int1_2 : GPIO.intr1_1.int1_1;
    lo &= s_core_lo[core];
    hi &= s_core_hi[core];
    if (lo) gpio_ll_clear_intr_status(&GPIO, lo);
    if (hi) gpio_ll_clear_intr_status_high(&GPIO, hi);
    for (int half = 0; half < 2; half++) {
        uint32_t bits = half ? hi : lo;
        while (bits) {
            int b = __builtin_ctz(bits);
            bits &= bits - 1;
            int pin = b + half * 32;
            struct papp_irq *h = s_pin_h[pin];
            if (!h || !h->enabled) continue;
            uint8_t level = gpio_ll_get_level(&GPIO, pin);
            if (h->trigger >= PAPP_IRQ_LOW) {            /* level: once, until enable() */
                GPIO.pin[pin].int_ena = 0;
                h->enabled = false;
            }
            fire(h, pin, level);
        }
    }
}

static bool IRAM_ATTR timer_alarm(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    (void)t; (void)e;
    struct papp_irq *h = arg;
    if (!h->dead) fire(h, 0xFF, 0);
    return false;                        /* push() yields itself */
}

/* ── Task side ───────────────────────────────────────────────────────────── */

typedef struct { void (*fn)(void *); void *arg; SemaphoreHandle_t done; } oncore_t;

static void oncore_task(void *p)
{
    oncore_t *c = p;
    c->fn(c->arg);
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

/* fn(arg) on a task pinned to core (internal stack); false if it could not be started */
static bool run_on_core(int core, void (*fn)(void *), void *arg)
{
    oncore_t c = { fn, arg, xSemaphoreCreateBinary() };
    bool ok = c.done && xTaskCreatePinnedToCore(oncore_task, "lx_irq", 4096, &c, configMAX_PRIORITIES - 2,
                                                NULL, core) == pdPASS;
    if (ok) xSemaphoreTake(c.done, portMAX_DELAY);
    else ESP_LOGE(TAG, "no memory for the core %d task", core);
    if (c.done) vSemaphoreDelete(c.done);
    return ok;
}

static int find_slot(const struct papp_irq *h)
{
    if (!h) return -1;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (s_h[i] == h && h->magic == MAGIC) return i;
    return -1;
}

static int count_kind(int kind)
{
    int n = 0;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (s_h[i] && s_h[i]->kind == kind) n++;
    return n;
}

static int check_cfg(const papp_irq_cfg_t *cfg, int *core, int *prio, int *qlen)
{
    papp_irq_cfg_t d = { .core = -1 };
    if (!cfg) cfg = &d;
    *core = cfg->core < 0 ? 1 : cfg->core;
    *prio = cfg->priority ? cfg->priority : 1;
    *qlen = cfg->queue ? cfg->queue : 16;
    if (*core > 1 || *prio > 3 || *qlen > MAX_QUEUE) return PAPP_E_ARG;
    if (cfg->isr) {
        uintptr_t base; size_t size, a = (uintptr_t)cfg->isr;
        if (!lx_app_fast_sram(&base, &size) || a < base || a >= base + size) {
            ESP_LOGW(TAG, "a direct handler must be PAPP_FAST code in internal SRAM");
            return PAPP_E_UNSUPPORTED;
        }
    }
    return PAPP_E_OK;
}

static struct papp_irq *new_handle(int kind, int core, int prio, int qlen, const papp_irq_cfg_t *cfg)
{
    struct papp_irq *h = heap_caps_calloc(1, sizeof(*h) + qlen * sizeof(papp_irq_event_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h) return NULL;
    h->magic = MAGIC;
    h->kind = kind;
    h->core = core;
    h->prio = prio;
    h->qlen = qlen;
    h->flags = cfg ? cfg->flags : 0;
    h->isr = cfg ? cfg->isr : NULL;
    h->isr_arg = cfg ? cfg->isr_arg : NULL;
    h->sem = xSemaphoreCreateCountingStatic(qlen, 0, &h->sem_buf);
    return h;
}

static bool add_handle(struct papp_irq *h)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!s_h[i]) {
            portENTER_CRITICAL(&s_spin);
            s_h[i] = h;
            portEXIT_CRITICAL(&s_spin);
            if (h->flags & PAPP_IRQ_AWAKE && s_awake[h->core]++ == 0 && s_awake_task[h->core])
                xTaskNotifyGive(s_awake_task[h->core]);
            return true;
        }
    return false;
}

/* PAPP_IRQ_AWAKE: a priority-1 task per core that spins while its core has such an interrupt. An interrupt
 * that finds the core in its idle task starts some 17 us late (measured on the Tab5: 20 us instead of
 * 3, up to ~70), whether the idle task sleeps (WFI) or not: keeping it from sleeping with an idle hook
 * changes nothing; any task above it does. Every 100 ms the spinner gives the core up for a tick, so
 * the idle task still runs (task watchdog, freeing deleted tasks). */
static void awake_task(void *arg)
{
    int core = (int)(intptr_t)arg;
    for (;;) {
        if (!s_awake[core]) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue; }
        int64_t end = esp_timer_get_time() + 100000;
        while (s_awake[core] && esp_timer_get_time() < end) { }
        vTaskDelay(1);
    }
}

static uint64_t irq_gpio_mask(void)
{
    SVC_ENTER();
    uint64_t m = s_mask;
    SVC_LEAVE();
    return m;
}

static int irq_gpio_config(int pin, int mode, int pull)
{
    if (!pin_in(s_mask, pin) || mode < 0 || mode > PAPP_GPIO_OPEN_DRAIN || pull < 0 || pull > PAPP_GPIO_PULL_DOWN)
        return PAPP_E_ARG;
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode == PAPP_GPIO_OUTPUT ? GPIO_MODE_INPUT_OUTPUT
              : mode == PAPP_GPIO_OPEN_DRAIN ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT,
        .pull_up_en = pull == PAPP_GPIO_PULL_UP,
        .pull_down_en = pull == PAPP_GPIO_PULL_DOWN,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&c);
    if (e == ESP_OK) {
        s_cfg |= 1ULL << pin;
        if (mode == PAPP_GPIO_INPUT) s_out &= ~(1ULL << pin);
        else s_out |= 1ULL << pin;
        if (s_pin_h[pin]) {                      /* gpio_config turned its interrupt type off */
            static const gpio_int_type_t t[] = { 0, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE,
                                                 GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL };
            gpio_ll_set_intr_type(&GPIO, pin, t[s_pin_h[pin]->trigger]);
            if (s_pin_h[pin]->enabled) GPIO.pin[pin].int_ena = s_ena_bit[s_pin_h[pin]->core];
        }
    }
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
    return e == ESP_OK ? PAPP_E_OK : PAPP_E_ARG;
}

static int irq_gpio_write(int pin, int level)
{
    if (!pin_in(s_out, pin)) return PAPP_E_ARG;
    SVC_ENTER();
    gpio_ll_set_level(&GPIO, pin, level ? 1 : 0);
    SVC_LEAVE();
    return PAPP_E_OK;
}

static int irq_gpio_read(int pin)
{
    if (!pin_in(s_cfg, pin)) return PAPP_E_ARG;
    SVC_ENTER();
    int v = gpio_ll_get_level(&GPIO, pin);
    SVC_LEAVE();
    return v;
}

/* on the pin's core: the dispatcher (first pin there) and the pin's enable */
typedef struct { struct papp_irq *h; int prio; esp_err_t err; bool on; } pin_op_t;

static void pin_attach_on_core(void *p)
{
    pin_op_t *o = p;
    int core = o->h->core;
    if (!s_gintr[core]) {
        o->err = esp_intr_alloc(s_source[core], ESP_INTR_FLAG_IRAM | (ESP_INTR_FLAG_LEVEL1 << (o->prio - 1)),
                                gpio_dispatch, (void *)(intptr_t)core, &s_gintr[core]);
        if (o->err != ESP_OK) { s_gintr[core] = NULL; return; }
    }
    int pin = o->h->pin;
    portENTER_CRITICAL(&s_spin);
    s_pin_h[pin] = o->h;
    if (pin < 32) s_core_lo[core] |= 1u << pin; else s_core_hi[core] |= 1u << (pin - 32);
    portEXIT_CRITICAL(&s_spin);
    o->err = ESP_OK;
}

static void pin_enable(struct papp_irq *h, bool on)
{
    int pin = h->pin;
    if (on) {
        if (pin < 32) gpio_ll_clear_intr_status(&GPIO, 1u << pin);
        else gpio_ll_clear_intr_status_high(&GPIO, 1u << (pin - 32));
        h->enabled = true;
        GPIO.pin[pin].int_ena = s_ena_bit[h->core];
    } else {
        GPIO.pin[pin].int_ena = 0;
        h->enabled = false;
    }
}

static void pin_detach_on_core(void *p)
{
    struct papp_irq *h = p;
    int pin = h->pin, core = h->core;
    GPIO.pin[pin].int_ena = 0;
    h->enabled = false;
    portENTER_CRITICAL(&s_spin);
    s_pin_h[pin] = NULL;
    if (pin < 32) s_core_lo[core] &= ~(1u << pin); else s_core_hi[core] &= ~(1u << (pin - 32));
    portEXIT_CRITICAL(&s_spin);
    gpio_ll_set_intr_type(&GPIO, pin, GPIO_INTR_DISABLE);
}

static int irq_gpio_attach(int pin, int trigger, const papp_irq_cfg_t *cfg, papp_irq_t **out)
{
    int core, prio, qlen;
    if (!out || !pin_in(s_cfg, pin) || trigger < PAPP_IRQ_RISING || trigger > PAPP_IRQ_HIGH) return PAPP_E_ARG;
    int r = check_cfg(cfg, &core, &prio, &qlen);
    if (r) return r;
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    r = PAPP_E_LIMIT;
    if (s_pin_h[pin] || count_kind(KIND_PIN) >= PAPP_IRQ_MAX_PINS) goto out;
    r = PAPP_E_NOMEM;
    struct papp_irq *h = new_handle(KIND_PIN, core, prio, qlen, cfg);
    if (!h) goto out;
    h->pin = pin;
    h->trigger = trigger;
    static const gpio_int_type_t t[] = { 0, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE,
                                         GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL };
    gpio_ll_set_intr_type(&GPIO, pin, t[trigger]);
    pin_op_t o = { .h = h, .prio = prio, .err = ESP_FAIL };
    add_handle(h);
    if (!run_on_core(core, pin_attach_on_core, &o) || o.err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d: no interrupt on core %d (%s)", pin, core, esp_err_to_name(o.err));
        portENTER_CRITICAL(&s_spin);
        for (int i = 0; i < MAX_HANDLES; i++) if (s_h[i] == h) s_h[i] = NULL;
        portEXIT_CRITICAL(&s_spin);
        if (h->flags & PAPP_IRQ_AWAKE) s_awake[core]--;
        gpio_ll_set_intr_type(&GPIO, pin, GPIO_INTR_DISABLE);
        heap_caps_free(h);
        r = PAPP_E_UNSUPPORTED;
        goto out;
    }
    pin_enable(h, true);
    *out = h;
    r = PAPP_E_OK;
    ESP_LOGI(TAG, "GPIO %d interrupt on core %d (%s)", pin, core, h->isr ? "direct" : "deferred");
out:
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
    return r;
}

typedef struct { struct papp_irq *h; esp_err_t err; } timer_op_t;

static void timer_new_on_core(void *p)
{
    timer_op_t *o = p;
    gptimer_config_t c = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = o->h->prio,
    };
    o->err = gptimer_new_timer(&c, &o->h->timer);
    if (o->err != ESP_OK) { o->h->timer = NULL; return; }
    gptimer_event_callbacks_t cb = { .on_alarm = timer_alarm };
    o->err = gptimer_register_event_callbacks(o->h->timer, &cb, o->h);
    if (o->err == ESP_OK) o->err = gptimer_enable(o->h->timer);
    if (o->err != ESP_OK) { gptimer_del_timer(o->h->timer); o->h->timer = NULL; }
}

static void timer_del_on_core(void *p)
{
    struct papp_irq *h = p;
    if (!h->timer) return;
    if (h->running) gptimer_stop(h->timer);
    h->running = false;
    gptimer_disable(h->timer);
    gptimer_del_timer(h->timer);
    h->timer = NULL;
}

static int irq_timer_attach(const papp_irq_cfg_t *cfg, papp_irq_t **out)
{
    int core, prio, qlen;
    if (!out) return PAPP_E_ARG;
    int r = check_cfg(cfg, &core, &prio, &qlen);
    if (r) return r;
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    r = PAPP_E_LIMIT;
    if (count_kind(KIND_TIMER) >= PAPP_IRQ_MAX_TIMERS) goto out;
    r = PAPP_E_NOMEM;
    struct papp_irq *h = new_handle(KIND_TIMER, core, prio, qlen, cfg);
    if (!h) goto out;
    h->pin = 0xFF;
    timer_op_t o = { .h = h, .err = ESP_FAIL };
    if (!run_on_core(core, timer_new_on_core, &o) || o.err != ESP_OK) {
        ESP_LOGW(TAG, "no hardware timer for the app (%s)", esp_err_to_name(o.err));
        heap_caps_free(h);
        r = o.err == ESP_ERR_NOT_FOUND ? PAPP_E_LIMIT : PAPP_E_UNSUPPORTED;
        goto out;
    }
    add_handle(h);
    *out = h;
    r = PAPP_E_OK;
    ESP_LOGI(TAG, "timer on core %d (%s)", core, h->isr ? "direct" : "deferred");
out:
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
    return r;
}

static int timer_start_locked(struct papp_irq *h, uint32_t us, bool periodic)
{
    if (h->running) { gptimer_stop(h->timer); h->running = false; }
    gptimer_set_raw_count(h->timer, 0);
    gptimer_alarm_config_t a = { .alarm_count = us, .reload_count = 0, .flags.auto_reload_on_alarm = periodic };
    if (gptimer_set_alarm_action(h->timer, &a) != ESP_OK) return PAPP_E_ARG;
    if (gptimer_start(h->timer) != ESP_OK) return PAPP_E_ARG;
    h->running = true;
    h->enabled = true;
    return PAPP_E_OK;
}

static int irq_timer_start(papp_irq_t *t, uint32_t us, bool periodic)
{
    if (!us) return PAPP_E_ARG;
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int r = find_slot(t) < 0 || t->kind != KIND_TIMER ? PAPP_E_ARG : timer_start_locked(t, us, periodic);
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
    return r;
}

static int irq_timer_stop(papp_irq_t *t)
{
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int r = PAPP_E_ARG;
    if (find_slot(t) >= 0 && t->kind == KIND_TIMER) {
        if (t->running) gptimer_stop(t->timer);
        t->running = false;
        t->enabled = false;
        r = PAPP_E_OK;
    }
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
    return r;
}

static int irq_enable(papp_irq_t *h, bool on)
{
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int r = PAPP_E_ARG;
    if (find_slot(h) >= 0) {
        r = PAPP_E_OK;
        if (h->kind == KIND_PIN) {
            pin_enable(h, on);
        } else if (on && !h->running) {
            r = gptimer_start(h->timer) == ESP_OK ? PAPP_E_OK : PAPP_E_ARG;   /* with its last alarm */
            if (!r) h->running = h->enabled = true;
        } else if (!on && h->running) {
            gptimer_stop(h->timer);
            h->running = h->enabled = false;
        }
    }
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
    return r;
}

static bool pop(struct papp_irq *h, papp_irq_event_t *ev)
{
    bool got = false;
    portENTER_CRITICAL(&s_spin);
    if (h->qn) {
        *ev = h->q[h->qhead];
        h->qhead = (h->qhead + 1) % h->qlen;
        h->qn--;
        got = true;
    }
    portEXIT_CRITICAL(&s_spin);
    return got;
}

static int irq_wait(papp_irq_t *h, papp_irq_event_t *ev, uint32_t timeout_ms)
{
    if (!ev) return PAPP_E_ARG;
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = find_slot(h) >= 0 && !h->dead;
    if (ok) h->waiters++;
    xSemaphoreGive(s_lock);
    if (!ok) { SVC_LEAVE(); return PAPP_E_ARG; }

    int r = PAPP_E_TIMEOUT;
    int64_t end = timeout_ms == PAPP_WAIT_FOREVER ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (pop(h, ev)) {
            xSemaphoreTake(h->sem, 0);           /* the give that came with it */
            r = 1;
            break;
        }
        if (h->dead || lx_svc_exit_flag) { r = PAPP_E_EXIT; break; }
        int64_t left = end - esp_timer_get_time();
        if (left <= 0) break;
        uint32_t ms = left > 50000 ? 50 : (uint32_t)((left + 999) / 1000);
        xSemaphoreTake(h->sem, pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
        if (pop(h, ev)) { r = 1; break; }    /* the give is taken: don't take another */
    }
    __atomic_sub_fetch(&h->waiters, 1, __ATOMIC_SEQ_CST);
    SVC_LEAVE();
    return r;
}

/* s_lock held. Interrupt off and gone (on its core), handle out of the table; freed unless a thread is
 * still in wait() on it (it wakes and leaves; the handle is leaked rather than freed under it). */
static void detach_locked(struct papp_irq *h, bool wait_waiters)
{
    h->dead = true;
    if (h->kind == KIND_PIN) run_on_core(h->core, pin_detach_on_core, h);
    else run_on_core(h->core, timer_del_on_core, h);
    portENTER_CRITICAL(&s_spin);
    for (int i = 0; i < MAX_HANDLES; i++) if (s_h[i] == h) s_h[i] = NULL;
    portEXIT_CRITICAL(&s_spin);
    if (h->flags & PAPP_IRQ_AWAKE) s_awake[h->core]--;
    xSemaphoreGive(h->sem);
    for (int i = 0; wait_waiters && h->waiters && i < 100; i++) {
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(5));
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (h->waiters) {
        ESP_LOGW(TAG, "a thread still waits on a detached interrupt; its handle is kept");
        return;
    }
    h->magic = 0;
    heap_caps_free(h);
}

static void irq_detach(papp_irq_t *h)
{
    SVC_ENTER();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (find_slot(h) >= 0) detach_locked(h, true);
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
}

static int irq_core(papp_irq_t *h)
{
    SVC_ENTER();
    int r = find_slot(h) >= 0 ? h->core : PAPP_E_ARG;
    SVC_LEAVE();
    return r;
}

const papp_if_irq_t lx_if_irq = {
    .hdr = { PAPP_IF_IRQ, PAPP_IF_IRQ_VER, sizeof(papp_if_irq_t) },
    .gpio_mask = irq_gpio_mask,
    .gpio_config = irq_gpio_config,
    .gpio_write = irq_gpio_write,
    .gpio_read = irq_gpio_read,
    .gpio_attach = irq_gpio_attach,
    .timer_attach = irq_timer_attach,
    .timer_start = irq_timer_start,
    .timer_stop = irq_timer_stop,
    .enable = irq_enable,
    .wait = irq_wait,
    .detach = irq_detach,
    .core = irq_core,
};

void lx_svc_irq_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int c = 0; c < 2; c++)
        xTaskCreatePinnedToCore(awake_task, "lx_awake", 2048, (void *)(intptr_t)c, 1, &s_awake_task[c], c);
}

void lx_svc_irq_begin_run(void)
{
    s_mask = bsp_app_gpio_mask();
    s_cfg = s_out = 0;
}

static void free_dispatcher(void *p)
{
    int core = (int)(intptr_t)p;
    esp_intr_free(s_gintr[core]);
    s_gintr[core] = NULL;
}

/* After the app's threads are gone: every interrupt freed, timers deleted, pins back to plain inputs. */
int lx_svc_irq_end_run(void)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_HANDLES; i++)
        if (s_h[i]) { detach_locked(s_h[i], false); n++; }
    for (int core = 0; core < 2; core++)
        if (s_gintr[core]) run_on_core(core, free_dispatcher, (void *)(intptr_t)core);
    for (int pin = 0; pin < NPINS; pin++)
        if ((s_cfg >> pin) & 1) {
            gpio_reset_pin(pin);                 /* input, output off; it turns the pull-up on */
            gpio_set_pull_mode(pin, GPIO_FLOATING);
        }
    if (s_cfg) ESP_LOGI(TAG, "pins 0x%llx back to inputs", (unsigned long long)s_cfg);
    s_cfg = s_out = 0;
    xSemaphoreGive(s_lock);
    if (n) ESP_LOGI(TAG, "freed %d interrupt(s) the app left attached", n);
    return n;
}

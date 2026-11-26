/* CoreMark port for a PAPP (perf_bench). See core_portme.h. */
#include "coremark.h"
#include "papp/papp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

/* What perf_bench reads after a run */
double cm_iter_per_sec;
int cm_valid;

static uint64_t s_start, s_stop;

void start_time(void) { s_start = (uint64_t)papp_time_us(); }
void stop_time(void)  { s_stop = (uint64_t)papp_time_us(); }
CORE_TICKS get_time(void) { return (CORE_TICKS)((s_stop - s_start) / 1000); }
secs_ret time_in_secs(CORE_TICKS ticks) { return (secs_ret)ticks / 1000.0; }

/* One papp_log line per text line; the result lines are also kept */
int ee_printf(const char *fmt, ...)
{
    static char line[192];
    static size_t n;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (const char *p = buf; *p; p++) {
        if (*p != '\n' && n < sizeof(line) - 1) {
            line[n++] = *p;
            continue;
        }
        if (*p != '\n') continue;
        line[n] = 0;
        n = 0;
        if (!strncmp(line, "Iterations/Sec", 14)) {
            const char *v = strchr(line, ':');
            if (v) sscanf(v + 1, "%lf", &cm_iter_per_sec);
        }
        if (strstr(line, "Correct operation validated")) cm_valid = 1;
        papp_log("cm: %s", line);
    }
    return len;
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
        ee_printf("ERROR! ee_ptr_int must hold a pointer\n");
    if (sizeof(ee_u32) != 4)
        ee_printf("ERROR! ee_u32 must be 32 bits\n");
    p->portable_id = 1;
}

void portable_fini(core_portable *p) { p->portable_id = 0; }

/* A context in a thread of its own, internal-SRAM stack */
static int ctx_main(void *arg)
{
    iterate(arg);
    return 0;
}

static int s_next_core;

ee_u8 core_start_parallel(core_results *res)
{
    /* Context 0 on core 1, context 1 on core 0. NORMAL priority: a HIGH thread on the creating
     * thread's core would run to its end before the creator could start the next context. */
    int core = (s_next_core++ & 1) ^ 1;
    papp_thread_cfg_t cfg = { .name = "coremark", .stack_size = 8192, .prio = PAPP_PRIO_NORMAL, .core = (int8_t)core };
    res->port.thread = papp_thread_create(ctx_main, res, &cfg);
    if (!res->port.thread) ee_printf("ERROR! no thread for context\n");
    return res->port.thread != NULL;
}

ee_u8 core_stop_parallel(core_results *res)
{
    if (res->port.thread) papp_thread_join(res->port.thread, PAPP_WAIT_FOREVER, NULL);
    res->port.thread = NULL;
    return 1;
}

void cm_reset_cores(void);
void cm_reset_cores(void) { s_next_core = 0; }     /* the same placement in every run */

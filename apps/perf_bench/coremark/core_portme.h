/* CoreMark port for a PAPP (perf_bench): time from papp_time_us, output through papp_log,
 * up to 2 contexts run as PAPP threads, one per core. Based on CoreMark's barebones port. */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>
#include <stdint.h>

#define HAS_FLOAT   1
#define HAS_TIME_H  0
#define USE_CLOCK   0
#define HAS_STDIO   0
#define HAS_PRINTF  0

#ifndef COMPILER_VERSION
#define COMPILER_VERSION "GCC" __VERSION__
#endif
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS "-O3 -funroll-all-loops -finline-limit=600 -fno-common"
#endif
#define MEM_LOCATION "internal SRAM (thread stack)"

typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef ee_u32         ee_ptr_int;
typedef size_t         ee_size_t;

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

#define CORETIMETYPE uint64_t
typedef ee_u32 CORE_TICKS;      /* milliseconds */

#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD  MEM_STACK

#define MULTITHREAD     2
#define USE_PTHREAD     0
#define USE_FORK        0
#define USE_SOCKET      0
#define PARALLEL_METHOD "PAPP threads"

#define MAIN_HAS_NOARGC   0
#define MAIN_HAS_NORETURN 0

typedef struct CORE_PORTABLE_S {
    ee_u8 portable_id;
    void *thread;
} core_portable;

extern ee_u32 default_num_contexts;     /* set by perf_bench before each run: 1 or 2 */

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#if (TOTAL_DATA_SIZE == 1200)
#define PROFILE_RUN 1
#elif (TOTAL_DATA_SIZE == 2000)
#define PERFORMANCE_RUN 1
#else
#define VALIDATION_RUN 1
#endif
#endif

int ee_printf(const char *fmt, ...);

#endif

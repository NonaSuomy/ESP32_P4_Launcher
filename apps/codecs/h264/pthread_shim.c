/*
 * The POSIX threads FFmpeg uses (frame threading), on the launcher's THRD interface.
 *
 * newlib's pthread types are 32-bit handles: here they hold pointers to the THRD objects, made on first use
 * (so the static initializers, all ones or zero, also work). Condition variables are a waiter count and a
 * counting semaphore: a wake-up can go to a thread that started waiting after the signal, which FFmpeg's
 * waits (always in a loop on their condition) do not mind.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/times.h>
#include "papp/papp.h"
#include "pthread_shim.h"

#define THRD papp_if_thrd()

static uint8_t s_prio = PAPP_PRIO_NORMAL;
static papp_mutex_t *s_init_lock;          /* lazy creation of mutexes / conds; pthread_once */

void shim_init(void)
{
    if (!s_init_lock) s_init_lock = THRD->mutex_create(true);
}

void shim_set_prio(int prio) { s_prio = (uint8_t)prio; }

/* -- Threads -- */

typedef struct {
    void *(*fn)(void *);
    void *arg;
    void *ret;
    papp_thread_t *t;
} shim_thread_t;

static int tramp(void *p)
{
    shim_thread_t *t = p;
    t->ret = t->fn(t->arg);
    return 0;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    shim_thread_t *t = calloc(1, sizeof(*t));
    if (!t) return EAGAIN;
    t->fn = fn;
    t->arg = arg;
    /* internal stacks (32 KB is the most THRD puts there): a frame is decoded on these */
    papp_thread_cfg_t cfg = { .name = "ffh264", .stack_size = 32 * 1024, .prio = s_prio, .core = -1 };
    t->t = papp_thread_create(tramp, t, &cfg);
    if (!t->t) {
        free(t);
        return EAGAIN;
    }
    *out = (pthread_t)(uintptr_t)t;
    return 0;
}

int pthread_join(pthread_t th, void **ret)
{
    shim_thread_t *t = (shim_thread_t *)(uintptr_t)th;
    if (!t) return EINVAL;
    THRD->thread_join(t->t, PAPP_WAIT_FOREVER, NULL);
    if (ret) *ret = t->ret;
    free(t);
    return 0;
}

pthread_t pthread_self(void) { return (pthread_t)(uintptr_t)THRD->thread_self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
int sched_yield(void) { THRD->yield(); return 0; }

int pthread_attr_init(pthread_attr_t *a) { memset(a, 0, sizeof(*a)); return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s) { return 0; }
int pthread_setname_np(pthread_t t, const char *name) { return 0; }

/* -- Mutexes -- */

static papp_mutex_t *mtx(pthread_mutex_t *m)
{
    papp_mutex_t *p = (papp_mutex_t *)(uintptr_t)*m;
    if (p && *m != (pthread_mutex_t)0xFFFFFFFF) return p;
    THRD->mutex_lock(s_init_lock, PAPP_WAIT_FOREVER);
    p = (papp_mutex_t *)(uintptr_t)*m;
    if (!p || *m == (pthread_mutex_t)0xFFFFFFFF) {
        p = THRD->mutex_create(false);
        *m = (pthread_mutex_t)(uintptr_t)p;
    }
    THRD->mutex_unlock(s_init_lock);
    return p;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    papp_mutex_t *p = THRD->mutex_create(false);
    if (!p) return ENOMEM;
    *m = (pthread_mutex_t)(uintptr_t)p;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    papp_mutex_t *p = (papp_mutex_t *)(uintptr_t)*m;
    if (p && *m != (pthread_mutex_t)0xFFFFFFFF) THRD->mutex_destroy(p);
    *m = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)    { THRD->mutex_lock(mtx(m), PAPP_WAIT_FOREVER); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) { return THRD->mutex_lock(mtx(m), 0) ? 0 : EBUSY; }
int pthread_mutex_unlock(pthread_mutex_t *m)  { THRD->mutex_unlock(mtx(m)); return 0; }

/* -- Condition variables -- */

typedef struct {
    papp_mutex_t *lock;
    papp_sem_t *sem;
    int waiters;
} shim_cond_t;

static shim_cond_t *cond_new(void)
{
    shim_cond_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->lock = THRD->mutex_create(false);
    c->sem = THRD->sem_create(0, 64);
    if (!c->lock || !c->sem) {
        if (c->lock) THRD->mutex_destroy(c->lock);
        if (c->sem) THRD->sem_destroy(c->sem);
        free(c);
        return NULL;
    }
    return c;
}

static shim_cond_t *cnd(pthread_cond_t *cv)
{
    shim_cond_t *c = (shim_cond_t *)(uintptr_t)*cv;
    if (c && *cv != (pthread_cond_t)0xFFFFFFFF) return c;
    THRD->mutex_lock(s_init_lock, PAPP_WAIT_FOREVER);
    c = (shim_cond_t *)(uintptr_t)*cv;
    if (!c || *cv == (pthread_cond_t)0xFFFFFFFF) {
        c = cond_new();
        *cv = (pthread_cond_t)(uintptr_t)c;
    }
    THRD->mutex_unlock(s_init_lock);
    return c;
}

int pthread_cond_init(pthread_cond_t *cv, const pthread_condattr_t *a)
{
    shim_cond_t *c = cond_new();
    if (!c) return ENOMEM;
    *cv = (pthread_cond_t)(uintptr_t)c;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cv)
{
    shim_cond_t *c = (shim_cond_t *)(uintptr_t)*cv;
    if (c && *cv != (pthread_cond_t)0xFFFFFFFF) {
        THRD->mutex_destroy(c->lock);
        THRD->sem_destroy(c->sem);
        free(c);
    }
    *cv = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *m)
{
    shim_cond_t *c = cnd(cv);
    THRD->mutex_lock(c->lock, PAPP_WAIT_FOREVER);
    c->waiters++;
    THRD->mutex_unlock(c->lock);
    pthread_mutex_unlock(m);
    THRD->sem_take(c->sem, PAPP_WAIT_FOREVER);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cv)
{
    shim_cond_t *c = cnd(cv);
    THRD->mutex_lock(c->lock, PAPP_WAIT_FOREVER);
    if (c->waiters > 0) {
        c->waiters--;
        THRD->sem_give(c->sem);
    }
    THRD->mutex_unlock(c->lock);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cv)
{
    shim_cond_t *c = cnd(cv);
    THRD->mutex_lock(c->lock, PAPP_WAIT_FOREVER);
    while (c->waiters > 0) {
        c->waiters--;
        THRD->sem_give(c->sem);
    }
    THRD->mutex_unlock(c->lock);
    return 0;
}

/* -- Once -- */

int pthread_once(pthread_once_t *o, void (*fn)(void))
{
    THRD->mutex_lock(s_init_lock, PAPP_WAIT_FOREVER);
    if (!o->init_executed) {
        o->init_executed = 1;
        fn();
    }
    THRD->mutex_unlock(s_init_lock);
    return 0;
}

/* -- Bits of POSIX the decoder asks for -- */

long sysconf(int name)
{
    return name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF ? 2 : -1;
}

int usleep(useconds_t us)
{
    papp_delay_ms((us + 999) / 1000);
    return 0;
}

/* Clocks (av_get_random_seed's fallback; nothing per frame) */
int clock_gettime(clockid_t id, struct timespec *ts)
{
    int64_t us = papp_time_us();
    ts->tv_sec = (time_t)(us / 1000000);
    ts->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}

clock_t _times(struct tms *t)
{
    clock_t c = (clock_t)(papp_time_us() / (1000000 / CLOCKS_PER_SEC));
    if (t) { t->tms_utime = c; t->tms_stime = t->tms_cutime = t->tms_cstime = 0; }
    return c;
}

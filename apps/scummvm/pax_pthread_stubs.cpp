/*
 * The PAX runs as one task inside the launcher.  The RISC-V libstdc++
 * supplied by ESP-IDF is built with pthread-aware locale/guard support, but
 * a standalone PAX does not link the full ESP-IDF pthread component.
 *
 * These deliberately minimal ABI-compatible hooks keep the single PAX task
 * freestanding.  ScummVM itself is built without worker threads, so mutexes
 * and condition variables need no scheduler implementation here.
 */

#include <stddef.h>

extern "C" {

int pthread_once(unsigned long *once_control, void (*init_routine)(void)) {
    if (once_control && *once_control == 0) {
        *once_control = 1;
        if (init_routine)
            init_routine();
    }
    return 0;
}

int pthread_mutexattr_init(void *) { return 0; }
int pthread_mutexattr_destroy(void *) { return 0; }
int pthread_mutexattr_settype(void *, int) { return 0; }
int pthread_mutex_init(void *, const void *) { return 0; }
int pthread_mutex_destroy(void *) { return 0; }
int pthread_mutex_lock(void *) { return 0; }
int pthread_mutex_unlock(void *) { return 0; }

int pthread_cond_init(void *, const void *) { return 0; }
int pthread_cond_destroy(void *) { return 0; }
int pthread_cond_wait(void *, void *) { return 0; }
int pthread_cond_signal(void *) { return 0; }
int pthread_cond_broadcast(void *) { return 0; }

int pthread_rwlock_destroy(void *) { return 0; }
int pthread_rwlock_rdlock(void *) { return 0; }
int pthread_rwlock_wrlock(void *) { return 0; }
int pthread_rwlock_unlock(void *) { return 0; }

static void *g_pax_thread_local;

void *pthread_getspecific(unsigned long) {
    return g_pax_thread_local;
}

int pthread_setspecific(unsigned long, const void *value) {
    g_pax_thread_local = const_cast<void *>(value);
    return 0;
}

int pthread_key_create(unsigned long *key, void (*)(void *)) {
    if (key)
        *key = 1;
    return 0;
}

int pthread_key_delete(unsigned long) { return 0; }

int pthread_create(unsigned long *, const void *, void *(*)(void *), void *) {
    return 1;
}

int pthread_detach(unsigned long) { return 1; }
int pthread_join(unsigned long, void **) { return 1; }

}

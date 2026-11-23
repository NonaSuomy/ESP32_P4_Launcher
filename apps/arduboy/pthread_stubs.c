/* The P4 app runs this emulator on one thread. These satisfy libstdc++'s
 * optional threading hooks; the simulator itself does not create threads. */
#include <stddef.h>

int pthread_mutex_init(void *mutex, const void *attr) { (void)mutex; (void)attr; return 0; }
int pthread_mutex_destroy(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_lock(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_unlock(void *mutex) { (void)mutex; return 0; }
int pthread_mutexattr_init(void *attr) { (void)attr; return 0; }
int pthread_mutexattr_destroy(void *attr) { (void)attr; return 0; }
int pthread_mutexattr_settype(void *attr, int type) { (void)attr; (void)type; return 0; }
int pthread_cond_init(void *condition, const void *attr) { (void)condition; (void)attr; return 0; }
int pthread_cond_broadcast(void *condition) { (void)condition; return 0; }
int pthread_cond_wait(void *condition, void *mutex) { (void)condition; (void)mutex; return 0; }
int pthread_once(int *once, void (*function)(void))
{
    if (once && !*once) {
        *once = 1;
        if (function) function();
    }
    return 0;
}
int pthread_key_create(unsigned *key, void (*destructor)(void *))
{
    (void)destructor;
    if (key) *key = 0;
    return 0;
}
int pthread_key_delete(unsigned key) { (void)key; return 0; }
void *pthread_getspecific(unsigned key) { (void)key; return NULL; }
int pthread_setspecific(unsigned key, const void *value) { (void)key; (void)value; return 0; }
int _getentropy(void *buffer, size_t length) { (void)buffer; (void)length; return -1; }

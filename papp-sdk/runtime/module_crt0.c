/*
 * Start-up of a codec module (papp_build --module): the launcher calls _papp_start(PAPP_MODULE_ARGC, argv)
 * once after loading it, on the thread of the app that opened the codec; argv[0] points at the
 * `const void *` that gets the module's table (papp_module_table(), which the module defines).
 * The module's destructors never run: it is unloaded with the app's memory when the app ends.
 */
#include "papp/papp.h"

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

const void *papp_module_table(void);
void papp_rt_init(void) __attribute__((weak));

__attribute__((section(".text.papp_entry"), used))
int _papp_start(int argc, char **argv)
{
    if (argc != PAPP_MODULE_ARGC || !argv || !argv[0]) return -1;
    if (papp_rt_init) papp_rt_init();
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();
    *(const void **)argv[0] = papp_module_table();
    return 0;
}

/* A module has no main to leave: papp_exit ends the app, from whichever thread */
void papp_exit(int code)
{
    const papp_if_thrd_t *thrd = papp_if_thrd();
    if (thrd) thrd->app_exit(code);
    for (;;) papp_delay_ms(1000);
}

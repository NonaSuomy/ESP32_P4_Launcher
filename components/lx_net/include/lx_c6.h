/* The ESP32-C6 companion's firmware: version and update (the work runs on the lx_net task, see lx_net.h) */
#pragma once

#include <stdbool.h>
#include <stddef.h>

const char *lx_c6_version(void);
const char *lx_c6_host_version(void);
bool lx_c6_update_supported(void);
bool lx_c6_check_image(const char *path, char *version, size_t vlen, char *why, size_t wlen);
int lx_c6_update_percent(void);
const char *lx_c6_update_error(void);

void lx_c6_query(void);                 /* lx_net task, radio started */
void lx_c6_update(const char *path);    /* lx_net task; restarts the board when it worked */

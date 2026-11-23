#pragma once
#include <stdlib.h>
static inline void esp_restart(void) { abort(); }
static inline unsigned esp_get_free_heap_size(void) { return 0; }

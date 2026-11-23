#pragma once

#include "ff.h"

typedef enum {
    RES_OK = 0,
    RES_ERROR = 1
} DRESULT;

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);

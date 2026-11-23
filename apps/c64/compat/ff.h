#pragma once

/* Only the FATFS data types touched by GnGeo's optional SD-streaming path.
 * PAX builds keep ROM regions in PSRAM, so the actual FatFs implementation
 * is intentionally not linked. */
#include <stdint.h>

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef uint32_t LBA_t;
typedef char TCHAR;

typedef enum {
    FR_OK = 0,
    FR_NO_FILE = 4
} FRESULT;

#define FA_READ 0x01
#define FS_FAT32 3

typedef struct FATFS {
    WORD ssize;
    WORD csize;
    LBA_t database;
    LBA_t fatbase;
    BYTE pdrv;
    BYTE fs_type;
    DWORD n_fatent;
} FATFS;

typedef struct FIL_OBJ {
    FATFS *fs;
    DWORD objsize;
    DWORD sclust;
} FIL_OBJ;

typedef struct FIL {
    FIL_OBJ obj;
} FIL;

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_close(FIL *fp);

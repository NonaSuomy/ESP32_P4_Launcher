/*
 * Zipped ROMs: read one member of a .zip (stored or deflate, no ZIP64, no encryption) with the
 * ROM's miniz inflater. Apps see a zipped ROM under a virtual name: "dir/Zelda.zip" with Zelda.nes
 * inside is opened as "dir/Zelda.nes" (the file service unpacks it on open; lx_ui's browser hands
 * the emulator that name). A real file of that name always wins over the zip.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     name[128];         /* member name inside the zip (may contain folders) */
    uint32_t usize;             /* uncompressed size */
    uint32_t csize;
    uint32_t crc;
    uint32_t hdr_off;           /* local header offset */
    uint16_t method;            /* 0 stored, 8 deflate */
} lx_zip_member_t;

/* First member (in central directory order, folders skipped) whose extension is in the
 * comma-separated list exts (".nes,.fds"; NULL or "" = any). */
bool lx_zip_find(const char *zip, const char *exts, lx_zip_member_t *m);

/* "dir/stem.ext" -> zip "dir/stem.zip" that exists and holds a member with extension .ext */
bool lx_zip_for(const char *path, char *zip, size_t len, lx_zip_member_t *m);

/* The member unpacked into a new PSRAM buffer of m->usize bytes (+1, zeroed), CRC checked; NULL on error */
void *lx_zip_unpack_mem(const char *zip, const lx_zip_member_t *m);

/* The member unpacked into the file out (written to out.part, then renamed), CRC checked; 0 = OK */
int lx_zip_unpack_file(const char *zip, const lx_zip_member_t *m, const char *out);

#ifdef __cplusplus
}
#endif

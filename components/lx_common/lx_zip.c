/*
 * Zipped ROMs (lx_zip.h): central directory lookup + inflate with the chip ROM's miniz (tinfl).
 * Reads go into 64-byte-aligned PSRAM buffers, so the SD card transfers them by DMA at full speed.
 */
#include "lx_zip.h"
#include "lx_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "lx_zip";

#define CHUNK     (64 * 1024)       /* read size */
#define MAX_CD    (1024 * 1024)

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void *ps_alloc(size_t n)
{
    return heap_caps_aligned_alloc(64, n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
}

/* The zip with a DMA-aligned stdio buffer: stdio's own isn't aligned, and the SD driver then
 * reads one sector at a time (1.5 MB/s instead of 15). zclose frees the buffer after fclose. */
static FILE *zopen(const char *path, void **vbuf)
{
    FILE *f = fopen(path, "rb");
    *vbuf = NULL;
    if (!f) return NULL;
    *vbuf = ps_alloc(CHUNK);
    if (*vbuf && setvbuf(f, *vbuf, _IOFBF, CHUNK) != 0) { heap_caps_free(*vbuf); *vbuf = NULL; }
    return f;
}

static void zclose(FILE *f, void *vbuf)
{
    if (f) fclose(f);
    heap_caps_free(vbuf);
}

static bool read_at(FILE *f, long off, void *buf, size_t n)
{
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* ext (".nes", with the dot) is in the comma-separated list */
static bool ext_in(const char *list, const char *ext)
{
    if (!list || !*list) return true;
    size_t n = strlen(ext);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == n && strncasecmp(p, ext, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static const char *name_ext(const char *name)
{
    const char *slash = strrchr(name, '/');
    const char *dot = strrchr(slash ? slash + 1 : name, '.');
    return dot ? dot : "";
}

bool lx_zip_find(const char *zip, const char *exts, lx_zip_member_t *m)
{
    void *vbuf;
    FILE *f = zopen(zip, &vbuf);
    if (!f) return false;
    bool ok = false;
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) != 0) goto out;
    long size = ftell(f);
    if (size < 22) goto out;

    /* End of central directory: the last 22 bytes, or up to 64 KB earlier with a comment */
    size_t tail = size < 22 + 65535 ? (size_t)size : 22 + 65535;
    buf = ps_alloc(tail);
    if (!buf || !read_at(f, size - (long)tail, buf, tail)) goto out;
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) goto out;
    uint16_t count = rd16(buf + eocd + 10);
    uint32_t cd_size = rd32(buf + eocd + 12), cd_off = rd32(buf + eocd + 16);
    if (cd_size > MAX_CD || cd_off == 0xffffffffu || (long)cd_off + (long)cd_size > size) goto out;
    heap_caps_free(buf);
    buf = ps_alloc(cd_size);
    if (!buf || !read_at(f, cd_off, buf, cd_size)) goto out;

    uint32_t p = 0;
    for (unsigned i = 0; i < count && p + 46 <= cd_size; i++) {
        const uint8_t *h = buf + p;
        if (rd32(h) != 0x02014b50) break;
        uint16_t flags = rd16(h + 8), method = rd16(h + 10);
        uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (p + 46 + nlen > cd_size) break;
        char name[sizeof(m->name)];
        size_t k = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
        memcpy(name, h + 46, k);
        name[k] = 0;
        p += 46 + nlen + xlen + clen;
        if (!k || name[k - 1] == '/' || (flags & 1) || (method != 0 && method != 8)) continue;
        if (k < nlen) continue;                 /* name too long to keep */
        if (!ext_in(exts, name_ext(name))) continue;
        uint32_t usize = rd32(h + 24), csize = rd32(h + 20), off = rd32(h + 42);
        if (usize == 0xffffffffu || csize == 0xffffffffu || off == 0xffffffffu) continue;   /* ZIP64 */
        memcpy(m->name, name, k + 1);
        m->usize = usize;
        m->csize = csize;
        m->crc = rd32(h + 16);
        m->hdr_off = off;
        m->method = method;
        ok = true;
        break;
    }
out:
    heap_caps_free(buf);
    zclose(f, vbuf);
    return ok;
}

bool lx_zip_for(const char *path, char *zip, size_t len, lx_zip_member_t *m)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash + 1 : path, '.');
    if (!dot || strcasecmp(dot, ".zip") == 0) return false;
    size_t stem = (size_t)(dot - path);
    if (stem + 5 > len) return false;
    memcpy(zip, path, stem);
    memcpy(zip + stem, ".zip", 5);
    struct stat st;
    if (stat(zip, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    char ext[16];
    snprintf(ext, sizeof(ext), "%s", dot);
    return lx_zip_find(zip, ext, m);
}

/* Start of the member's data (after its local header), or -1 */
static long data_start(FILE *f, const lx_zip_member_t *m)
{
    uint8_t h[30];
    if (!read_at(f, m->hdr_off, h, sizeof(h)) || rd32(h) != 0x04034b50) return -1;
    return (long)m->hdr_off + 30 + rd16(h + 26) + rd16(h + 28);
}

/* Internal RAM when there is some: the inflater's Huffman tables and its 32 KB window are hit for
 * every byte */
static void *fast_alloc(size_t n)
{
    void *p = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > n + 64 * 1024)      /* leave the SD card's DMA its room */
        p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

/* Unpack to out (a whole buffer of m->usize) or, when out is NULL, to the file dst */
static bool unpack(FILE *f, const lx_zip_member_t *m, uint8_t *out, FILE *dst)
{
    long start = data_start(f, m);
    if (start < 0 || fseek(f, start, SEEK_SET) != 0) return false;
    uint8_t *in = ps_alloc(CHUNK);
    uint8_t *ring = m->method == 8 ? fast_alloc(TINFL_LZ_DICT_SIZE) : NULL;
    tinfl_decompressor *d = m->method == 8 ? fast_alloc(sizeof(*d)) : NULL;
    bool ok = false;
    if (!in || (m->method == 8 && (!d || !ring))) goto done;

    uint32_t crc = 0, left = m->csize, pos = 0;
    if (m->method == 0) {
        if (m->csize != m->usize) goto done;
        while (left) {
            size_t n = left < CHUNK ? left : CHUNK;
            uint8_t *to = out ? out + pos : in;
            if (fread(to, 1, n, f) != n) goto done;
            crc = lx_crc32(crc, to, n);
            if (dst && fwrite(to, 1, n, dst) != n) goto done;
            left -= n;
            pos += n;
        }
        ok = crc == m->crc;
        goto done;
    }

    /* Through the window as a ring: each stretch it produces is copied out at once */
    const uint32_t mask = TINFL_LZ_DICT_SIZE - 1;
    tinfl_init(d);
    size_t avail = 0, at = 0;
    for (;;) {
        if (!avail && left) {
            size_t n = left < CHUNK ? left : CHUNK;
            if (fread(in, 1, n, f) != n) goto done;
            left -= n;
            at = 0;
            avail = n;
        }
        size_t in_n = avail, out_n = TINFL_LZ_DICT_SIZE - (pos & mask);
        uint8_t *next = ring + (pos & mask);
        tinfl_status st = tinfl_decompress(d, in + at, &in_n, ring, next, &out_n,
                                           left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        at += in_n;
        avail -= in_n;
        if (pos + out_n > m->usize) goto done;
        crc = lx_crc32(crc, next, out_n);
        if (out) memcpy(out + pos, next, out_n);
        else if (out_n && fwrite(next, 1, out_n, dst) != out_n) goto done;
        pos += out_n;
        if (st == TINFL_STATUS_DONE) break;
        if (st < 0) goto done;
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !avail && !left) goto done;
    }
    ok = pos == m->usize && crc == m->crc;
done:
    if (!ok) ESP_LOGW(TAG, "%s: unpack failed", m->name);
    heap_caps_free(d);
    heap_caps_free(ring);
    heap_caps_free(in);
    return ok;
}

void *lx_zip_unpack_mem(const char *zip, const lx_zip_member_t *m)
{
    uint8_t *out = ps_alloc((size_t)m->usize + 1);
    if (!out) return NULL;
    out[m->usize] = 0;
    void *vbuf;
    FILE *f = zopen(zip, &vbuf);
    bool ok = f && unpack(f, m, out, NULL);
    zclose(f, vbuf);
    if (!ok) { heap_caps_free(out); out = NULL; }
    return out;
}

int lx_zip_unpack_file(const char *zip, const lx_zip_member_t *m, const char *out)
{
    char part[300];
    snprintf(part, sizeof(part), "%s.part", out);
    void *zbuf;
    FILE *f = zopen(zip, &zbuf);
    if (!f) return -1;
    FILE *dst = fopen(part, "wb");
    if (!dst) { zclose(f, zbuf); return -1; }
    void *vbuf = ps_alloc(CHUNK);               /* the window's short stretches go out in big DMA blocks */
    if (vbuf) setvbuf(dst, vbuf, _IOFBF, CHUNK);
    bool ok = unpack(f, m, NULL, dst);
    zclose(f, zbuf);
    if (fclose(dst) != 0) ok = false;
    heap_caps_free(vbuf);
    if (ok) {
        remove(out);
        ok = rename(part, out) == 0;
    }
    if (!ok) remove(part);
    return ok ? 0 : -1;
}

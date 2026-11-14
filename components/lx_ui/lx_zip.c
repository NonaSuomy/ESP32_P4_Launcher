#include "lx_zip.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "zlib.h"
#include "lx_ui.h"

#define ZIP_TAIL_MAX (22u + 65535u)
#define ZIP_INPUT_SIZE 4096u
#define ZIP_OUTPUT_SIZE 8192u
#define ZIP_FILE_BUF_SIZE (16u * 1024u)
#define ZIP_MAX_ROM_SIZE (512u * 1024u * 1024u)
#define ZIP_CACHE_DIR "/sd/launcher/zipcache"
#define ZIP_PROGRESS_STEP (256u * 1024u)

static uint16_t u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t archive_hash(const char *archive)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)archive; *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

static void make_cache_stem(const char *archive, char *stem, size_t stem_size)
{
    const char *base = strrchr(archive, '/');
    base = base ? base + 1 : archive;
    size_t length = strlen(base);
    if (length >= 4 && strcasecmp(base + length - 4, ".zip") == 0) length -= 4;
    size_t used = 0;
    while (used < length && used < 40 && used + 1 < stem_size - 10) {
        unsigned char c = (unsigned char)base[used];
        stem[used] = (isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
        ++used;
    }
    if (!used) {
        memcpy(stem, "rom", 3);
        used = 3;
    }
    snprintf(stem + used, stem_size - used, "-%08lx", (unsigned long)archive_hash(archive));
}

static int ext_matches(const char *name, const char *list, char *out, size_t cap)
{
    const char *base = strrchr(name, '/');
    const char *dot = strrchr(base ? base + 1 : name, '.');
    if (!dot || !dot[1]) return 0;
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') ++p;
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n && p[n - 1] == ' ') --n;
        if (n && p[0] == '.' && strlen(dot) == n && strncasecmp(p, dot, n) == 0) {
            if (n >= cap) return 0;
            for (size_t i = 0; i < n; ++i) out[i] = (char)tolower((unsigned char)p[i]);
            out[n] = 0;
            return 1;
        }
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static int safe_stem(const char *s)
{
    if (!s || !*s || strlen(s) > 64 || strstr(s, "..")) return 0;
    for (; *s; ++s)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-')) return 0;
    return 1;
}

static int ensure_cache_dir(void)
{
    struct stat st;
    if (stat("/sd/launcher", &st) != 0 && mkdir("/sd/launcher", 0775) != 0) return -1;
    if (stat("/sd/launcher", &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    if (stat(ZIP_CACHE_DIR, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(ZIP_CACHE_DIR, 0775);
}

static int clear_cache(void)
{
    DIR *dir = opendir(ZIP_CACHE_DIR);
    if (!dir) return -1;
    struct dirent *entry = NULL;
    char path[256];
    int result = 0;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno) result = -1;
            break;
        }
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;
        int n = snprintf(path, sizeof(path), ZIP_CACHE_DIR "/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path) || (remove(path) != 0 && errno != ENOENT))
            result = -1;
    }
    closedir(dir);
    return result;
}

static void *fast_stdio_buffer(FILE *file)
{
    void *buffer = heap_caps_aligned_alloc(64, ZIP_FILE_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (buffer && setvbuf(file, buffer, _IOFBF, ZIP_FILE_BUF_SIZE) != 0) {
        heap_caps_free(buffer);
        buffer = NULL;
    }
    return buffer;
}

static void *zip_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static int zip_first_extension(FILE *zf, long archive_size, const char *extensions,
                               char *extension, size_t extension_size)
{
    size_t tail_size = (size_t)(archive_size < (long)ZIP_TAIL_MAX ? archive_size : ZIP_TAIL_MAX);
    uint8_t *tail = heap_caps_malloc(tail_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tail) tail = malloc(tail_size);
    if (!tail || fseek(zf, archive_size - (long)tail_size, SEEK_SET) != 0 ||
        fread(tail, 1, tail_size, zf) != tail_size) {
        free(tail);
        return -1;
    }
    long eocd = -1;
    for (long i = (long)tail_size - 22; i >= 0; --i) {
        if (u32(tail + i) == 0x06054b50u && i + 22L + u16(tail + i + 20) <= (long)tail_size) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) { free(tail); return -1; }
    const uint8_t *end = tail + eocd;
    uint16_t entries = u16(end + 10);
    uint32_t central_size = u32(end + 12), central_offset = u32(end + 16);
    if (u16(end + 4) || u16(end + 6) || u16(end + 8) != entries || entries == 0xffff ||
        central_size == 0xffffffffu || central_offset == 0xffffffffu ||
        (uint64_t)central_offset + central_size > (uint64_t)archive_size ||
        fseek(zf, (long)central_offset, SEEK_SET) != 0) {
        free(tail);
        return -1;
    }
    int found = -1;
    for (uint32_t i = 0; i < entries; ++i) {
        uint8_t hdr[46];
        if (fread(hdr, 1, sizeof(hdr), zf) != sizeof(hdr) || u32(hdr) != 0x02014b50u) break;
        uint16_t name_len = u16(hdr + 28), extra_len = u16(hdr + 30), comment_len = u16(hdr + 32);
        if (!name_len || name_len > 4096) break;
        char *name = malloc((size_t)name_len + 1);
        if (!name) break;
        bool valid_name = fread(name, 1, name_len, zf) == name_len && !memchr(name, 0, name_len);
        name[name_len] = 0;
        if (!valid_name) { free(name); break; }
        uint16_t method = u16(hdr + 10);
        uint32_t compressed = u32(hdr + 20), uncompressed = u32(hdr + 24);
        int matched = !(u16(hdr + 8) & 1u) && (method == 0 || method == 8) &&
                      compressed <= ZIP_MAX_ROM_SIZE && uncompressed <= ZIP_MAX_ROM_SIZE &&
                      (method != 0 || compressed == uncompressed) &&
                      ext_matches(name, extensions, extension, extension_size);
        free(name);
        if (matched) {
            found = 0;
            break;
        }
        if (fseek(zf, (long)extra_len + comment_len, SEEK_CUR) != 0) break;
    }
    free(tail);
    return found;
}

int lx_zip_resolve_first(const char *archive, const char *extensions,
                         char *out_path, size_t out_size)
{
    if (!archive || strncmp(archive, "/sd/", 4) || !extensions || !*extensions ||
        !out_path || !out_size) return -1;
    FILE *zf = fopen(archive, "rb");
    if (!zf || fseek(zf, 0, SEEK_END) != 0) { if (zf) fclose(zf); return -1; }
    long size = ftell(zf);
    char extension[24] = "";
    int rc = size >= 22 ? zip_first_extension(zf, size, extensions, extension, sizeof(extension)) : -1;
    fclose(zf);
    if (rc != 0) return -1;
    char stem[64];
    make_cache_stem(archive, stem, sizeof(stem));
    int n = snprintf(out_path, out_size, ZIP_CACHE_DIR "/%s%s", stem, extension);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

int lx_zip_extract_first(const char *archive, const char *extensions,
                         volatile bool *abort, char *out_path, size_t out_size)
{
    if (!archive || strncmp(archive, "/sd/", 4) || !extensions || !*extensions ||
        !out_path || !out_size || ensure_cache_dir() != 0)
        return -1;

    char final_path[128] = {0}, part_path[136] = {0};
    FILE *zf = NULL, *of = NULL;
    int result = -1;
    uint8_t *tail = NULL, *in = NULL, *out = NULL;
    char *name = NULL;
    void *zf_buffer = NULL, *of_buffer = NULL;
    z_stream stream = {0};
    int inflater = 0;
    char cache_stem[64];
    make_cache_stem(archive, cache_stem, sizeof(cache_stem));
    if (!safe_stem(cache_stem)) goto done;
    zf = fopen(archive, "rb");
    if (zf) zf_buffer = fast_stdio_buffer(zf);
    if (!zf || fseek(zf, 0, SEEK_END) != 0) goto done;
    long archive_size = ftell(zf);
    if (archive_size < 22) goto done;
    size_t tail_size = (size_t)(archive_size < (long)ZIP_TAIL_MAX ? archive_size : ZIP_TAIL_MAX);
    tail = zip_alloc(tail_size);
    if (!tail || fseek(zf, archive_size - (long)tail_size, SEEK_SET) != 0 ||
        fread(tail, 1, tail_size, zf) != tail_size) goto done;

    long eocd = -1;
    for (long i = (long)tail_size - 22; i >= 0; --i) {
        if (u32(tail + i) == 0x06054b50u && i + 22L + u16(tail + i + 20) <= (long)tail_size) {
            eocd = i; break;
        }
    }
    if (eocd < 0) goto done;
    const uint8_t *end = tail + eocd;
    uint16_t disk = u16(end + 4), central_disk = u16(end + 6);
    uint16_t disk_entries = u16(end + 8), entries = u16(end + 10);
    uint32_t central_size = u32(end + 12), central_offset = u32(end + 16);
    if (disk || central_disk || disk_entries != entries || entries == 0xffff ||
        central_size == 0xffffffffu || central_offset == 0xffffffffu ||
        (uint64_t)central_offset + central_size > (uint64_t)archive_size) goto done;

    uint16_t method = 0, flags = 0;
    uint32_t compressed = 0, uncompressed = 0, expected_crc = 0, local_offset = 0;
    char extension[24] = "";
    int found = 0;
    if (fseek(zf, (long)central_offset, SEEK_SET) != 0) goto done;
    for (uint32_t i = 0; i < entries; ++i) {
        if (abort && *abort) goto done;
        uint8_t hdr[46];
        if (fread(hdr, 1, sizeof(hdr), zf) != sizeof(hdr) || u32(hdr) != 0x02014b50u) goto done;
        uint16_t name_len = u16(hdr + 28), extra_len = u16(hdr + 30), comment_len = u16(hdr + 32);
        if (!name_len || name_len > 4096) goto done;
        if (!name) name = zip_alloc(4097);
        if (!name) goto done;
        if (fread(name, 1, name_len, zf) != name_len || memchr(name, 0, name_len)) goto done;
        name[name_len] = 0;
        if (!found && ext_matches(name, extensions, extension, sizeof(extension))) {
            uint16_t entry_flags = u16(hdr + 8), entry_method = u16(hdr + 10);
            uint32_t entry_compressed = u32(hdr + 20), entry_uncompressed = u32(hdr + 24);
            if (!(entry_flags & 1u) && (entry_method == 0 || entry_method == 8) &&
                (entry_method != 0 || entry_compressed == entry_uncompressed) &&
                entry_compressed <= ZIP_MAX_ROM_SIZE && entry_uncompressed <= ZIP_MAX_ROM_SIZE) {
                flags = entry_flags; method = entry_method;
                expected_crc = u32(hdr + 16); compressed = entry_compressed;
                uncompressed = entry_uncompressed; local_offset = u32(hdr + 42);
                found = 1;
            }
        }
        if (fseek(zf, (long)extra_len + comment_len, SEEK_CUR) != 0) goto done;
    }
    if (!found || (flags & 1u) || (method != 0 && method != 8) ||
        (method == 0 && compressed != uncompressed)) goto done;

    if (clear_cache() != 0) goto done;
    if (snprintf(final_path, sizeof(final_path), ZIP_CACHE_DIR "/%s%s", cache_stem, extension) >= (int)sizeof(final_path) ||
        strlen(final_path) + sizeof(".part") > sizeof(part_path) || strlen(final_path) + 1 > out_size) goto done;
    snprintf(part_path, sizeof(part_path), "%s.part", final_path);
    remove(part_path);
    of = fopen(part_path, "wb");
    if (!of) goto done;
    of_buffer = fast_stdio_buffer(of);

    uint8_t local[30];
    if ((uint64_t)local_offset + sizeof(local) > (uint64_t)archive_size ||
        fseek(zf, (long)local_offset, SEEK_SET) != 0 || fread(local, 1, sizeof(local), zf) != sizeof(local) ||
        u32(local) != 0x04034b50u || u16(local + 6) != flags || u16(local + 8) != method) goto done;
    uint16_t local_name_len = u16(local + 26), local_extra_len = u16(local + 28);
    if (local_name_len > 4096 || fseek(zf, (long)local_name_len + local_extra_len, SEEK_CUR) != 0) goto done;
    uint64_t data_offset = (uint64_t)local_offset + sizeof(local) + local_name_len + local_extra_len;
    if (data_offset + compressed > (uint64_t)archive_size || data_offset > LONG_MAX ||
        fseek(zf, (long)data_offset, SEEK_SET) != 0) goto done;

    in = zip_alloc(ZIP_INPUT_SIZE);
    out = zip_alloc(ZIP_OUTPUT_SIZE);
    if (!in || !out) goto done;
    uint32_t remaining = compressed;
    uint64_t written = 0, next_progress = 0;
    uLong crc = crc32(0, Z_NULL, 0);
    int stream_result = Z_OK;
    lx_ui_progress("Preparing ROM", 0, uncompressed);
    if (method == 8) {
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) goto done;
        inflater = 1;
    }
    while (written < uncompressed || (method == 8 && stream_result != Z_STREAM_END)) {
        if (abort && *abort) goto done;
        if (method == 0) {
            size_t want = remaining < ZIP_INPUT_SIZE ? remaining : ZIP_INPUT_SIZE;
            if (!want) break;
            size_t got = fread(in, 1, want, zf);
            if (!got || fwrite(in, 1, got, of) != got) goto done;
            crc = crc32(crc, in, (uInt)got);
            remaining -= (uint32_t)got; written += got;
        } else {
            if (!stream.avail_in && remaining) {
                size_t want = remaining < ZIP_INPUT_SIZE ? remaining : ZIP_INPUT_SIZE;
                size_t got = fread(in, 1, want, zf);
                if (!got) goto done;
                stream.next_in = in; stream.avail_in = (uInt)got;
                remaining -= (uint32_t)got;
            }
            stream.next_out = out; stream.avail_out = ZIP_OUTPUT_SIZE;
            stream_result = inflate(&stream, Z_NO_FLUSH);
            size_t produced = ZIP_OUTPUT_SIZE - stream.avail_out;
            if (produced) {
                if (written + produced > uncompressed || fwrite(out, 1, produced, of) != produced) goto done;
                crc = crc32(crc, out, (uInt)produced);
                written += produced;
            }
            if (stream_result != Z_OK && stream_result != Z_STREAM_END) goto done;
            if (stream_result == Z_OK && !produced && !stream.avail_in && !remaining) goto done;
        }
        if (written >= next_progress || written == uncompressed) {
            lx_ui_progress("Preparing ROM", written, uncompressed);
            next_progress = written + ZIP_PROGRESS_STEP;
        }
    }
    if (written != uncompressed || remaining != 0 || (method == 8 && stream.avail_in != 0) ||
        crc != expected_crc || fflush(of) != 0 || fsync(fileno(of)) != 0) goto done;
    if (fclose(of) != 0) { of = NULL; goto done; }
    of = NULL;
    remove(final_path);
    if (rename(part_path, final_path) != 0) goto done;
    memcpy(out_path, final_path, strlen(final_path) + 1);
    result = 0;

done:
    if (inflater) inflateEnd(&stream);
    if (of) fclose(of);
    if (of_buffer) heap_caps_free(of_buffer);
    if (result != 0 && part_path[0]) remove(part_path);
    if (zf) fclose(zf);
    if (zf_buffer) heap_caps_free(zf_buffer);
    free(in);
    free(out);
    free(name);
    free(tail);
    return result;
}

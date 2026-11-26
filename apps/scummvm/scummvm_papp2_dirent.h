#pragma once

#include "scummvm_papp2_api.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Newlib's standalone app environment has no mounted POSIX directory stream.
 * Adapt ScummVM's expected opendir/readdir calls to the launcher's file-list
 * service while keeping each open directory's listing independent. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *listing;
    size_t offset;
    int remaining;
    int count;
    struct dirent entry;
} scummvm_dir_t;

DIR *opendir(const char *path) {
    if (!_pax_svc || !_pax_svc->file_list_dir || !path) {
        errno = ENOSYS;
        return NULL;
    }
    scummvm_dir_t *dir = (scummvm_dir_t *)calloc(1, sizeof(*dir));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }
    dir->listing = (char *)calloc(1, 65536);
    if (!dir->listing) {
        free(dir);
        errno = ENOMEM;
        return NULL;
    }
    dir->count = _pax_svc->file_list_dir(path, dir->listing, 65536);
    if (dir->count < 0) {
        free(dir->listing);
        free(dir);
        errno = ENOENT;
        return NULL;
    }
    dir->remaining = dir->count;
    return (DIR *)dir;
}

struct dirent *readdir(DIR *handle) {
    if (!handle) {
        errno = EBADF;
        return NULL;
    }
    scummvm_dir_t *dir = (scummvm_dir_t *)handle;
    if (dir->remaining <= 0)
        return NULL;
    const char *name = dir->listing + dir->offset;
    size_t length = strlen(name);
    dir->offset += length + 1;
    dir->remaining--;
    memset(&dir->entry, 0, sizeof(dir->entry));
    const int is_dir = length > 0 && name[length - 1] == '/';
    if (is_dir)
        length--;
    if (length >= sizeof(dir->entry.d_name))
        length = sizeof(dir->entry.d_name) - 1;
    memcpy(dir->entry.d_name, name, length);
    dir->entry.d_type = is_dir ? DT_DIR : DT_REG;
    return &dir->entry;
}

int closedir(DIR *handle) {
    if (!handle) {
        errno = EBADF;
        return -1;
    }
    scummvm_dir_t *dir = (scummvm_dir_t *)handle;
    free(dir->listing);
    free(dir);
    return 0;
}

void rewinddir(DIR *handle) {
    if (!handle)
        return;
    scummvm_dir_t *dir = (scummvm_dir_t *)handle;
    dir->offset = 0;
    dir->remaining = dir->count;
}

long telldir(DIR *handle) {
    if (!handle) {
        errno = EBADF;
        return -1;
    }
    return ((scummvm_dir_t *)handle)->count - ((scummvm_dir_t *)handle)->remaining;
}

void seekdir(DIR *handle, long location) {
    if (!handle || location < 0)
        return;
    rewinddir(handle);
    while (location-- > 0 && readdir(handle)) {}
}

#ifdef __cplusplus
}
#endif

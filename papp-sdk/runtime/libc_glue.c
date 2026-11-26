/*
 * newlib glue for apps that link the toolchain libc (-lc -lm).
 *
 * One shared implementation of what every RetroESP32_P4 port carried in its
 * own papp_syscalls.c: heap routing (via --wrap), file descriptors over the FS
 * interface, stdout/stderr to the launcher log, re-entrancy struct, exit.
 * Link flags are added by papp_build.py (--libc).
 */
#include "papp/papp.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <reent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

#undef errno
extern int errno;

/* ── Re-entrancy: one struct _reent per thread, kept in the THRD TLS slot ─── */

static struct _reent s_reent;              /* main thread (and launchers without THRD) */
static papp_mutex_t *s_libc_lock;          /* see Locks below */

struct _reent *__getreent(void)
{
    const papp_if_thrd_t *thrd = papp_if_thrd();
    void **slot = thrd ? thrd->tls_slot() : NULL;
    if (!slot) return &s_reent;
    if (!*slot) {
        struct _reent *r = papp_alloc(sizeof(*r), PAPP_MEM_ANY, 0);
        if (!r) return &s_reent;           /* out of memory: share main's (racy but alive) */
        _REENT_INIT_PTR(r);
        *slot = r;
    }
    return *slot;
}

void papp_rt_init(void)
{
    _REENT_INIT_PTR(&s_reent);
    const papp_if_thrd_t *thrd = papp_if_thrd();
    if (thrd) {
        void **slot = thrd->tls_slot();
        if (slot) *slot = &s_reent;
        s_libc_lock = thrd->mutex_create(true);
    }
}

/* Called by the SDK when a worker thread ends. */
void papp_rt_thread_end(void)
{
    void **slot = papp_if_thrd()->tls_slot();
    struct _reent *r = slot ? *slot : NULL;
    if (!r || r == &s_reent) return;
    _reclaim_reent(r);
    papp_free(r);
    *slot = NULL;
}

/* ── Heap: everything goes to the launcher MEM interface (tracked per app) ── */

void *__wrap_malloc(size_t n)                      { return papp_alloc(n, PAPP_MEM_ANY, 0); }
void  __wrap_free(void *p)                         { papp_free(p); }
void *__wrap_realloc(void *p, size_t n)            { return papp_realloc(p, n); }
void *__wrap_memalign(size_t a, size_t n)          { return papp_alloc(n, PAPP_MEM_ANY, a); }

void *__wrap_calloc(size_t c, size_t n)
{
    size_t total;
    if (__builtin_mul_overflow(c, n, &total)) return NULL;
    void *p = papp_alloc(total, PAPP_MEM_ANY, 0);
    if (p) memset(p, 0, total);
    return p;
}

void *__wrap__malloc_r(struct _reent *r, size_t n)            { return __wrap_malloc(n); }
void  __wrap__free_r(struct _reent *r, void *p)               { __wrap_free(p); }
void *__wrap__realloc_r(struct _reent *r, void *p, size_t n)  { return __wrap_realloc(p, n); }
void *__wrap__calloc_r(struct _reent *r, size_t c, size_t n)  { return __wrap_calloc(c, n); }
void *__wrap__memalign_r(struct _reent *r, size_t a, size_t n){ return __wrap_memalign(a, n); }

void *_sbrk(ptrdiff_t incr)
{
    errno = ENOMEM;
    return (void *)-1;
}

/* ── Locks ──────────────────────────────────────────────────────────────────
 * Every newlib lock (stdio files, sfp list, env, tz, at-exit...) maps to one
 * recursive app mutex. Newlib's static locks are tiny structs we cannot put
 * a handle in, and the heap doesn't go through newlib, so the contention a
 * single lock adds is only on stdio. No THRD: no threads, so no-ops. */

struct __lock;
static void lock_take(void)  { if (s_libc_lock) papp_if_thrd()->mutex_lock(s_libc_lock, PAPP_WAIT_FOREVER); }
static int  lock_try(void)   { return !s_libc_lock || papp_if_thrd()->mutex_lock(s_libc_lock, 0); }
static void lock_give(void)  { if (s_libc_lock) papp_if_thrd()->mutex_unlock(s_libc_lock); }

void __wrap___retarget_lock_init(struct __lock **l)               { if (l) *l = 0; }
void __wrap___retarget_lock_init_recursive(struct __lock **l)     { if (l) *l = 0; }
void __wrap___retarget_lock_close(struct __lock *l)               {}
void __wrap___retarget_lock_close_recursive(struct __lock *l)     {}
void __wrap___retarget_lock_acquire(struct __lock *l)             { lock_take(); }
void __wrap___retarget_lock_acquire_recursive(struct __lock *l)   { lock_take(); }
int  __wrap___retarget_lock_try_acquire(struct __lock *l)         { return lock_try(); }
int  __wrap___retarget_lock_try_acquire_recursive(struct __lock *l) { return lock_try(); }
void __wrap___retarget_lock_release(struct __lock *l)             { lock_give(); }
void __wrap___retarget_lock_release_recursive(struct __lock *l)   { lock_give(); }

/* ── File descriptors: 0-2 = console, 3.. = FS files ────────────────────── */

#define FD_BASE  3
#define FD_MAX   32
static papp_file_t *s_fds[FD_MAX];

static papp_file_t *fd_file(int fd)
{
    return (fd >= FD_BASE && fd < FD_BASE + FD_MAX) ? s_fds[fd - FD_BASE] : NULL;
}

static const char *open_mode(int flags)
{
    switch (flags & O_ACCMODE) {
    case O_RDONLY: return "rb";
    case O_WRONLY: return (flags & O_APPEND) ? "ab" : "wb";
    default:
        if (flags & O_APPEND) return "a+b";
        return (flags & (O_CREAT | O_TRUNC)) ? "w+b" : "r+b";
    }
}

int _open(const char *path, int flags, int mode)
{
    const papp_if_fs_t *fs = papp_fs();
    if (!fs) { errno = ENOSYS; return -1; }
    papp_file_t *f = fs->open(path, open_mode(flags));
    if (!f) { errno = ENOENT; return -1; }
    lock_take();                           /* the fd table is shared by all threads */
    for (int i = 0; i < FD_MAX; i++) {
        if (!s_fds[i]) {
            s_fds[i] = f;
            lock_give();
            return FD_BASE + i;
        }
    }
    lock_give();
    fs->close(f);
    errno = EMFILE;
    return -1;
}

int _close(int fd)
{
    lock_take();
    papp_file_t *f = fd_file(fd);
    if (f) s_fds[fd - FD_BASE] = NULL;
    lock_give();
    if (!f) return fd < FD_BASE ? 0 : (errno = EBADF, -1);
    return papp_fs()->close(f);
}

ssize_t _write(int fd, const void *buf, size_t n)
{
    if (fd == 1 || fd == 2) {
        papp_if_sys()->log_write(buf, n);
        return n;
    }
    papp_file_t *f = fd_file(fd);
    if (!f) { errno = EBADF; return -1; }
    return papp_fs()->write(f, buf, n);
}

ssize_t _read(int fd, void *buf, size_t n)
{
    if (fd == 0) return 0;
    papp_file_t *f = fd_file(fd);
    if (!f) { errno = EBADF; return -1; }
    return papp_fs()->read(f, buf, n);
}

off_t _lseek(int fd, off_t off, int whence)
{
    papp_file_t *f = fd_file(fd);
    if (!f) { errno = EBADF; return -1; }
    if (papp_fs()->seek(f, off, whence) != 0) { errno = EINVAL; return -1; }
    return papp_fs()->tell(f);
}

int _fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fd < FD_BASE) { st->st_mode = S_IFCHR; return 0; }
    papp_file_t *f = fd_file(fd);
    if (!f) { errno = EBADF; return -1; }
    const papp_if_fs_t *fs = papp_fs();
    long cur = fs->tell(f);
    fs->seek(f, 0, SEEK_END);
    st->st_size = fs->tell(f);           /* real size: newlib's fseek(SEEK_END) relies on it */
    fs->seek(f, cur, SEEK_SET);
    st->st_mode = S_IFREG;
    st->st_blksize = 512;
    return 0;
}

/* FS v2: stat with directories, rename, rmdir, directory listing. */
static const papp_if_fs_t *fs2(void)
{
    const papp_if_fs_t *fs = papp_fs();
    return fs && fs->hdr.version >= 2 ? fs : NULL;
}

int _stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fs2()) {
        papp_stat_t s;
        if (fs2()->stat(path, &s) != 0) { errno = ENOENT; return -1; }
        st->st_size = (off_t)s.size;
        st->st_mode = s.is_dir ? S_IFDIR | 0775 : S_IFREG | 0664;
        st->st_mtime = (time_t)s.mtime;
        st->st_blksize = 512;
        return 0;
    }
    long sz = papp_fs() ? papp_fs()->size(path) : -1;
    if (sz < 0) { errno = ENOENT; return -1; }
    st->st_size = sz;
    st->st_mode = S_IFREG;
    return 0;
}

int _isatty(int fd)                   { return fd >= 0 && fd < FD_BASE; }
int _unlink(const char *path)         { return papp_fs() && papp_fs()->remove(path) == 0 ? 0 : (errno = ENOENT, -1); }
int _link(const char *a, const char *b) { errno = ENOSYS; return -1; }
int _rename(const char *a, const char *b)
{
    if (!fs2()) { errno = ENOSYS; return -1; }                             /* a launcher before FS v2 */
    return fs2()->rename(a, b) == 0 ? 0 : (errno = EIO, -1);
}
int rmdir(const char *path)
{
    if (!fs2()) { errno = ENOSYS; return -1; }
    return fs2()->rmdir(path) == 0 ? 0 : (errno = ENOTEMPTY, -1);
}

/* newlib's DIR is the VFS's header; ours carries the launcher's handle and the entry readdir returns. */
typedef struct {
    DIR hdr;
    papp_dir_t *d;
    struct dirent e;
} papp_DIR;

DIR *opendir(const char *path)
{
    if (!fs2()) { errno = ENOSYS; return NULL; }
    papp_dir_t *d = fs2()->opendir(path);
    if (!d) { errno = ENOENT; return NULL; }
    papp_DIR *pd = calloc(1, sizeof(*pd));
    if (!pd) { fs2()->closedir(d); errno = ENOMEM; return NULL; }
    pd->d = d;
    return &pd->hdr;
}

struct dirent *readdir(DIR *dir)
{
    papp_DIR *pd = (papp_DIR *)dir;
    papp_dirent_t e;
    if (!pd || !fs2() || fs2()->readdir(pd->d, &e) != 1) return NULL;
    memset(&pd->e, 0, sizeof(pd->e));
    strncpy(pd->e.d_name, e.name, sizeof(pd->e.d_name) - 1);
    pd->e.d_type = e.is_dir ? DT_DIR : DT_REG;
    return &pd->e;
}

int closedir(DIR *dir)
{
    papp_DIR *pd = (papp_DIR *)dir;
    if (!pd) return -1;
    if (fs2()) fs2()->closedir(pd->d);
    free(pd);
    return 0;
}
int _fcntl(int fd, int cmd, int arg)  { return 0; }
int fsync(int fd)                     { return 0; }                         /* writes go straight to the FS */
int _kill(int pid, int sig)           { errno = EINVAL; return -1; }
int _getpid(void)                     { return 1; }
/* getentropy(): the hardware random number generator (SYS v5), for libraries that ask the C library. */
int getentropy(void *buf, size_t len)
{
    if (len > 256) { errno = EIO; return -1; }
    return papp_random(buf, len) ? 0 : (errno = ENOSYS, -1);
}

int mkdir(const char *path, mode_t m) { return papp_fs() && papp_fs()->mkdir(path) == 0 ? 0 : -1; }

int _gettimeofday(struct timeval *tv, void *tz)
{
    if (tv) {
        int64_t us = papp_time_us();
        tv->tv_sec = us / 1000000;
        tv->tv_usec = us % 1000000;
    }
    return 0;
}

/* usleep() / sleep() in newlib end up here */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req) { errno = EINVAL; return -1; }
    uint64_t us = (uint64_t)req->tv_sec * 1000000u + req->tv_nsec / 1000;
    papp_delay_ms((uint32_t)((us + 999) / 1000));
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

void _exit(int code)
{
    papp_exit(code);
}

/* Called from main()'s caller path only via exit()/abort(): close leftovers. */
__attribute__((destructor)) static void close_fds(void)
{
    for (int i = 0; i < FD_MAX; i++)
        if (s_fds[i]) { papp_fs()->close(s_fds[i]); s_fds[i] = NULL; }
}

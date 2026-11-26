/*
 * ScummVM AGI/SCI PAPP2 backend.
 *
 * The ScummVM engine libraries are built once for the P4 and this file is
 * the small platform layer that connects them to the launcher's append-only
 * PAPP2 ABI.  The same source is used for both scummvm-agi.pax and
 * scummvm-sci.pax; only the selected ScummVM engine archive changes.
 */
#define PAX_APP_SIDE 1
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#ifdef SCUMMVM_PAPP2_NATIVE
#include "scummvm_papp2_api.h"
#define app_services_t ScummVMPlatformApi
#else
#include "psram_app.h"
#endif
#include "config.h"
#include "base/main.h"
#include "common/system.h"
#include "common/config-manager.h"
#include "common/fs.h"
#include "common/archive.h"
#include "common/compression/unzip.h"
#include "common/textconsole.h"
#include "common/rect.h"
#include "common/mutex.h"
#include "common/memstream.h"
#include "backends/modular-backend.h"
#include "backends/graphics/graphics.h"
#include "backends/fs/fs-factory.h"
#include "backends/fs/posix/posix-fs.h"
#include "backends/mixer/mixer.h"
#include "audio/mixer_intern.h"
#include "graphics/surface.h"
#include "backends/events/default/default-events.h"
#include "backends/timer/default/default-timer.h"
#include "backends/saves/default/default-saves.h"
#include "backends/keymapper/hardware-input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

extern "C" { const app_services_t *_pax_svc = NULL; }

/* The launcher selects the storage volume at runtime.  Keep the PAX's
 * historical /sd namespace available through the service resolver, but use
 * the actual selected mount for ScummVM's own filesystem nodes and path
 * settings.  This makes the paths shown in Global Settings match the volume
 * that is really being used and avoids depending on /sd being mounted. */
static char s_scummvm_storage_root[8] = "/sd";
static bool s_scummvm_storage_root_ready = false;

static const char *scummvm_storage_root() {
	if (s_scummvm_storage_root_ready)
		return s_scummvm_storage_root;

	const char *root = "/sd";
	if (_pax_svc && _pax_svc->file_stat) {
		pax_file_stat_t info{};
		if (_pax_svc->file_stat("/usb0/roms", &info) == 0 && info.is_dir)
			root = "/usb0";
	}
	strncpy(s_scummvm_storage_root, root, sizeof(s_scummvm_storage_root) - 1);
	s_scummvm_storage_root[sizeof(s_scummvm_storage_root) - 1] = '\0';
	s_scummvm_storage_root_ready = true;
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("ScummVM storage root selected: %s\n",
			s_scummvm_storage_root);
	return s_scummvm_storage_root;
}

static bool scummvm_storage_path(char *out, size_t out_size, const char *suffix) {
	if (!out || out_size == 0 || !suffix)
		return false;
	int written = snprintf(out, out_size, "%s%s%s", scummvm_storage_root(),
		suffix[0] == '/' ? "" : "/", suffix);
	return written >= 0 && (size_t)written < out_size;
}

static bool scummvm_rebase_path(const char *input, char *out, size_t out_size) {
	if (!input || !out || out_size == 0)
		return false;

	const char *old_roots[] = { "/sd", "/usb0" };
	for (const char *old_root : old_roots) {
		size_t root_len = strlen(old_root);
		if (strncmp(input, old_root, root_len) != 0 ||
			(input[root_len] != '\0' && input[root_len] != '/'))
			continue;
		int written = snprintf(out, out_size, "%s%s", scummvm_storage_root(),
			input + root_len);
		return written >= 0 && (size_t)written < out_size;
	}
	return false;
}

static void scummvm_rebase_domain_path(Common::ConfigManager::Domain *domain,
		const char *key, const char *domain_name) {
	if (!domain || !key || !domain->contains(key))
		return;
	Common::Path old_path = Common::Path::fromConfig(domain->getVal(key));
	Common::String old_string = old_path.toString('/');
	char new_path[256];
	if (!scummvm_rebase_path(old_string.c_str(), new_path, sizeof(new_path)) ||
		old_string == new_path)
		return;
	domain->setVal(key, Common::Path(new_path).toConfig());
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("ScummVM path rebased %s/%s: %s -> %s\n",
			domain_name ? domain_name : "?", key, old_string.c_str(), new_path);
}

static void scummvm_rebase_config_paths() {
	static const char *const application_keys[] = {
		"path", "savepath", "extrapath", "iconspath", "pluginspath",
		"themepath", "browser_lastpath"
	};
	Common::ConfigManager::Domain *application =
		ConfMan.getDomain(Common::ConfigManager::kApplicationDomain);
	for (const char *key : application_keys)
		scummvm_rebase_domain_path(application, key,
			Common::ConfigManager::kApplicationDomain);

	for (Common::ConfigManager::DomainMap::iterator domain = ConfMan.beginGameDomains();
		 domain != ConfMan.endGameDomains(); ++domain) {
		scummvm_rebase_domain_path(&domain->_value, "path", domain->_key.c_str());
	}
}

static jmp_buf s_exit_env;
static volatile int s_exit_requested;
class PaxMixerManager;
static PaxMixerManager *s_pax_mixer_manager;
static void scummvm_stop_audio_task();

extern "C" void app_return_to_launcher(void) {
	 s_exit_requested = 1;
	 /* Stop the mixer callback before longjmp skips ScummVM's normal engine
	  * teardown. Otherwise it can mix from sound channels while the engine is
	  * unwinding and fault on a freed/null stream. */
	 scummvm_stop_audio_task();
	 longjmp(s_exit_env, 1);
}

/* ── C++ allocation and exit hooks ───────────────────────────────────── */

struct pax_alloc_header {
	size_t size;
	uint32_t magic;
	uint32_t reserved[2];
};

static const uint32_t kPaxAllocMagic = 0x50415050U;

static void *pax_alloc(size_t size) {
	if (!_pax_svc) return NULL;
	/* ScummVM creates a very large number of small C++ objects while it
	 * builds the launcher and engine state. Keeping those small allocations
	 * on the internal heap exhausts it before a game starts, so use PSRAM for
	 * the complete ScummVM heap as well as its large buffers. */
	/* Keep the requested size beside each PSRAM allocation so realloc can
	 * copy only the bytes that actually belong to the old object.  The old
	 * wrapper copied the new size unconditionally, corrupting newer ScummVM
	 * containers as they grew. */
	size_t requested = size ? size : 1;
	if (requested > SIZE_MAX - sizeof(pax_alloc_header)) return NULL;
	pax_alloc_header *header = (pax_alloc_header *)_pax_svc->mem_caps_alloc(
		requested + sizeof(pax_alloc_header), PAX_MEM_CAP_SPIRAM);
	if (!header) return NULL;
	header->size = requested;
	header->magic = kPaxAllocMagic;
	header->reserved[0] = 0;
	header->reserved[1] = 0;
	return header + 1;
}

static bool pax_owned(void *ptr, size_t *size = NULL) {
	if (!ptr) return false;
	pax_alloc_header *header = ((pax_alloc_header *)ptr) - 1;
	if (header->magic != kPaxAllocMagic) return false;
	if (size) *size = header->size;
	return true;
}

static void pax_free(void *ptr) {
	if (!ptr || !_pax_svc) return;
	if (pax_owned(ptr))
		_pax_svc->mem_free(((pax_alloc_header *)ptr) - 1);
	else
		_pax_svc->mem_free(ptr);
}

static void *pax_realloc(void *old, size_t size) {
	if (!old) return pax_alloc(size);
	if (size == 0) {
		pax_free(old);
		return NULL;
	}
	size_t old_size = 0;
	if (!pax_owned(old, &old_size))
		return _pax_svc ? _pax_svc->mem_realloc(old, size) : NULL;
	void *replacement = pax_alloc(size);
	if (!replacement) return NULL;
	memcpy(replacement, old, old_size < size ? old_size : size);
	pax_free(old);
	return replacement;
}

#ifdef SCUMMVM_PAPP2_NATIVE
void *operator new(size_t size) { return pax_alloc(size); }
void *operator new[](size_t size) { return pax_alloc(size); }
#else
void *operator new(size_t size) noexcept { return pax_alloc(size); }
void *operator new[](size_t size) noexcept { return pax_alloc(size); }
#endif
void operator delete(void *p) noexcept { pax_free(p); }
void operator delete[](void *p) noexcept { pax_free(p); }
void operator delete(void *p, size_t) noexcept { pax_free(p); }
void operator delete[](void *p, size_t) noexcept { pax_free(p); }

extern "C" void *__wrap_malloc(size_t size) { return pax_alloc(size); }
extern "C" void *__wrap_calloc(size_t n, size_t size) {
	size_t total = n * size;
	void *p = pax_alloc(total);
	if (p) memset(p, 0, total);
	return p;
}
extern "C" void *__wrap_realloc(void *old, size_t size) {
	return pax_realloc(old, size);
}
extern "C" void __wrap_free(void *p) { pax_free(p); }
extern "C" void *__wrap__malloc_r(struct _reent *, size_t n) { return __wrap_malloc(n); }
extern "C" void __wrap__free_r(struct _reent *, void *p) { __wrap_free(p); }
extern "C" void *__wrap__calloc_r(struct _reent *, size_t n, size_t s) { return __wrap_calloc(n, s); }
extern "C" void *__wrap__realloc_r(struct _reent *, void *p, size_t n) { return __wrap_realloc(p, n); }
extern "C" void *__wrap__memalign_r(struct _reent *, size_t, size_t n) { return __wrap_malloc(n); }

extern "C" void _exit(int) { app_return_to_launcher(); for (;;) {} }
extern "C" void __wrap_exit(int status) { _exit(status); }
extern "C" void abort(void) {
	if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM PAX abort()\n");
	_exit(1);
}

/* newlib's reentrancy/lock hooks are not useful inside one PAX task. */
struct __lock;
extern "C" void __wrap___retarget_lock_init(struct __lock **p) { if (p) *p = NULL; }
extern "C" void __wrap___retarget_lock_init_recursive(struct __lock **p) { if (p) *p = NULL; }
extern "C" void __wrap___retarget_lock_close(struct __lock *) {}
extern "C" void __wrap___retarget_lock_close_recursive(struct __lock *) {}
extern "C" void __wrap___retarget_lock_acquire(struct __lock *) {}
extern "C" int __wrap___retarget_lock_try_acquire(struct __lock *) { return 0; }
extern "C" void __wrap___retarget_lock_acquire_recursive(struct __lock *) {}
extern "C" int __wrap___retarget_lock_try_acquire_recursive(struct __lock *) { return 0; }
extern "C" void __wrap___retarget_lock_release(struct __lock *) {}
extern "C" void __wrap___retarget_lock_release_recursive(struct __lock *) {}
extern "C" struct _reent *__getreent(void) { return _impure_ptr; }

/* ── stdio wrappers ─────────────────────────────────────────────────── */

/* The FILE objects belong to the launcher's VFS service, so ScummVM cannot
 * safely rely on the app-side libc's feof/ferror state.  Keep the standard
 * indicators at the PAX boundary instead.  SCI's resource-map parser uses
 * eos() while scanning variable-length map directories; returning a false
 * EOF here makes a valid map look corrupt and eventually produces
 * "GfxDefaultDriver: Unknown view type". */
struct pax_file_state {
	FILE *file;
	unsigned eof : 1;
	unsigned error : 1;
};

static pax_file_state s_file_states[64];

static pax_file_state *pax_file_state_for(FILE *file, bool create) {
	if (!file)
		return NULL;
	pax_file_state *free_slot = NULL;
	for (unsigned i = 0; i < sizeof(s_file_states) / sizeof(s_file_states[0]); ++i) {
		if (s_file_states[i].file == file)
			return &s_file_states[i];
		if (!s_file_states[i].file && !free_slot)
			free_slot = &s_file_states[i];
	}
	if (!create || !free_slot)
		return NULL;
	free_slot->file = file;
	free_slot->eof = 0;
	free_slot->error = 0;
	return free_slot;
}

static void pax_file_state_remove(FILE *file) {
	pax_file_state *state = pax_file_state_for(file, false);
	if (state)
		memset(state, 0, sizeof(*state));
}

static bool scummvm_save_path(const char *path) {
	return path && (strstr(path, "/scummvm/saves/") != NULL ||
		strstr(path, "/scummvm.ini") != NULL);
}

extern "C" FILE *__wrap_fopen(const char *path, const char *mode) {
	FILE *file = _pax_svc && _pax_svc->file_open ?
		(FILE *)_pax_svc->file_open(path, mode) : NULL;
	if (file)
		pax_file_state_for(file, true);
	if (scummvm_save_path(path) && mode && strpbrk(mode, "wa+")) {
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM save open %s mode=%s -> %s errno=%d\n",
				path, mode, file ? "ok" : "failed", file ? 0 : errno);
	}
	return file;
}
extern "C" int __wrap_fclose(FILE *f) {
	int result = (_pax_svc && f) ? _pax_svc->file_close(f) : EOF;
	pax_file_state_remove(f);
	return result;
}
extern "C" size_t __wrap_fread(void *p, size_t s, size_t n, FILE *f) {
	if (!_pax_svc || !f) return 0;
	size_t got = _pax_svc->file_read(p, s, n, f);
	pax_file_state *state = pax_file_state_for(f, true);
	if (state && n > 0 && got < n)
		state->eof = 1;
	return got;
}
extern "C" size_t __wrap_fwrite(const void *p, size_t s, size_t n, FILE *f) {
	size_t written = (_pax_svc && f) ? _pax_svc->file_write(p, s, n, f) : 0;
	if (written != n) {
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM file write short: requested=%u written=%u\n",
				(unsigned)n, (unsigned)written);
	}
	return written;
}
extern "C" int __wrap_fseek(FILE *f, long off, int whence) {
	if (!_pax_svc || !f) return -1;
	int result = _pax_svc->file_seek(f, off, whence);
	pax_file_state *state = pax_file_state_for(f, true);
	if (state && result == 0) {
		state->eof = 0;
		state->error = 0;
	}
	static unsigned trace_seeks = 0;
	if (trace_seeks < 32 && (off < 0 || whence != SEEK_SET || result != 0)) {
		++trace_seeks;
		if (_pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM fseek off=%ld whence=%d result=%d\n",
				off, whence, result);
	}
	return result;
}
extern "C" long __wrap_ftell(FILE *f) {
	return (_pax_svc && f) ? _pax_svc->file_tell(f) : -1;
}
extern "C" int __wrap_getc(FILE *f) {
	unsigned char c = 0;
	if (!_pax_svc || !f)
		return EOF;
	if (_pax_svc->file_read(&c, 1, 1, f) == 1)
		return c;
	pax_file_state *state = pax_file_state_for(f, true);
	if (state)
		state->eof = 1;
	return EOF;
}
extern "C" int __wrap_fgetc(FILE *f) { return __wrap_getc(f); }
/* FILE handles returned by the PAX service are launcher-owned opaque
 * objects, not the PAX libc's FILE layout.  ScummVM's direct-launch
 * sidecar and a few POSIX helpers use fgets(), so letting newlib's native
 * implementation inspect this handle causes a load fault inside memchr().
 * Read through the service-backed getc wrapper instead. */
extern "C" char *__wrap_fgets(char *buffer, int size, FILE *f) {
	if (!buffer || size <= 0 || !_pax_svc || !f)
		return NULL;
	int length = 0;
	while (length < size - 1) {
		int c = __wrap_getc(f);
		if (c == EOF)
			break;
		buffer[length++] = (char)c;
		if (c == '\n')
			break;
	}
	if (length == 0)
		return NULL;
	buffer[length] = '\0';
	return buffer;
}
extern "C" int __wrap_feof(FILE *f) {
	pax_file_state *state = pax_file_state_for(f, false);
	return state ? (int)state->eof : 0;
}
extern "C" int __wrap_ferror(FILE *f) {
	pax_file_state *state = pax_file_state_for(f, false);
	return state ? (int)state->error : 0;
}
extern "C" void __wrap_clearerr(FILE *f) {
	pax_file_state *state = pax_file_state_for(f, false);
	if (state) {
		state->eof = 0;
		state->error = 0;
	}
}
extern "C" int __wrap_fflush(FILE *) { return 0; }

/* ── POSIX/newlib filesystem facade ─────────────────────────────────── */

#include "scummvm_papp2_dirent.h"

static int pax_stat_path(const char *path, struct stat *st) {
	if (!_pax_svc || !_pax_svc->file_stat || !st) return -1;
	pax_file_stat_t info{};
	if (_pax_svc->file_stat(path, &info) != 0) return -1;
	memset(st, 0, sizeof(*st));
	st->st_mode = info.is_dir ? S_IFDIR : S_IFREG;
	st->st_size = info.size;
	return 0;
}
extern "C" int stat(const char *path, struct stat *st) { return pax_stat_path(path, st); }
extern "C" int lstat(const char *path, struct stat *st) { return pax_stat_path(path, st); }
extern "C" int _stat(const char *path, struct stat *st) { return pax_stat_path(path, st); }
extern "C" int _stat_r(struct _reent *, const char *path, struct stat *st) { return pax_stat_path(path, st); }
extern "C" int access(const char *path, int) {
	struct stat st;
	return pax_stat_path(path, &st);
}
extern "C" int mkdir(const char *path, mode_t) {
	return (_pax_svc && _pax_svc->file_mkdir) ? _pax_svc->file_mkdir(path) : -1;
}
extern "C" int rmdir(const char *) { return 0; }
extern "C" int unlink(const char *path) {
	if (_pax_svc && _pax_svc->file_remove)
		return _pax_svc->file_remove(path);
	errno = EROFS;
	return -1;
}
extern "C" int _unlink(const char *p) { return unlink(p); }
extern "C" int _unlink_r(struct _reent *, const char *p) { return unlink(p); }
extern "C" int remove(const char *path) { return unlink(path); }
extern "C" int rename(const char *src, const char *dst) {
	if (_pax_svc && _pax_svc->file_rename)
		return _pax_svc->file_rename(src, dst);
	errno = EROFS;
	return -1;
}
extern "C" char *getcwd(char *buf, size_t size) {
	if (!buf || size < 5) return NULL;
	if (!scummvm_storage_path(buf, size, "/")) return NULL;
	return buf;
}

static int fd_flags_mode(int flags) {
	return (flags & O_ACCMODE) == O_WRONLY ? 1 :
	       ((flags & O_ACCMODE) == O_RDWR ? 2 : 0);
}
/* Keep the PAX-side POSIX descriptor table ahead of the USB FAT VFS limit.
 * Current ScummVM builds can have launcher, theme, and game streams open at
 * the same time. */
static void *s_fd_table[32];
static int fd_index(int fd) { return fd - 3; }
extern "C" int _open(const char *path, int flags, int) {
	if (!_pax_svc || !_pax_svc->file_open) return -1;
	const char *mode = fd_flags_mode(flags) == 2 ? ((flags & O_CREAT) ? "wb+" : "rb+") :
	                   (fd_flags_mode(flags) == 1 ? "wb" : "rb");
	void *f = _pax_svc->file_open(path, mode);
	if (!f) return -1;
	for (int i = 0; i < 32; ++i) if (!s_fd_table[i]) { s_fd_table[i] = f; return i + 3; }
	_pax_svc->file_close(f);
	return -1;
}
extern "C" int _open_r(struct _reent *, const char *path, int flags, int mode) { return _open(path, flags, mode); }
extern "C" int _close(int fd) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return -1;
	int r = _pax_svc->file_close(s_fd_table[i]);
	s_fd_table[i] = NULL;
	return r;
}
extern "C" int _close_r(struct _reent *, int fd) { return _close(fd); }
extern "C" ssize_t _read(int fd, void *buf, size_t count) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return -1;
	return (ssize_t)_pax_svc->file_read(buf, 1, count, s_fd_table[i]);
}
extern "C" ssize_t _read_r(struct _reent *, int fd, void *buf, size_t count) { return _read(fd, buf, count); }
extern "C" ssize_t _write(int fd, const void *buf, size_t count) {
	if (fd == 1 || fd == 2) {
		if (_pax_svc && _pax_svc->log_printf) {
			char tmp[256]; size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
			memcpy(tmp, buf, n); tmp[n] = '\0'; _pax_svc->log_printf("%s", tmp);
		}
		return (ssize_t)count;
	}
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return -1;
	return (ssize_t)_pax_svc->file_write(buf, 1, count, s_fd_table[i]);
}
extern "C" ssize_t _write_r(struct _reent *, int fd, const void *buf, size_t count) { return _write(fd, buf, count); }
extern "C" off_t _lseek(int fd, off_t off, int whence) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return (off_t)-1;
	if (_pax_svc->file_seek(s_fd_table[i], (long)off, whence) != 0) return (off_t)-1;
	return (off_t)_pax_svc->file_tell(s_fd_table[i]);
}
extern "C" off_t _lseek_r(struct _reent *, int fd, off_t off, int whence) { return _lseek(fd, off, whence); }
extern "C" int _fstat(int fd, struct stat *st) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i] || !st) return -1;
	memset(st, 0, sizeof(*st)); st->st_mode = S_IFREG;
	long cur = _pax_svc->file_tell(s_fd_table[i]);
	_pax_svc->file_seek(s_fd_table[i], 0, SEEK_END); st->st_size = _pax_svc->file_tell(s_fd_table[i]);
	_pax_svc->file_seek(s_fd_table[i], cur, SEEK_SET);
	return 0;
}
extern "C" int _fstat_r(struct _reent *, int fd, struct stat *st) { return _fstat(fd, st); }
extern "C" int _isatty(int fd) { return fd >= 0 && fd <= 2; }
extern "C" int _isatty_r(struct _reent *, int fd) { return _isatty(fd); }
extern "C" void *_sbrk(ptrdiff_t) { return (void *)-1; }
extern "C" void *_sbrk_r(struct _reent *, ptrdiff_t) { return (void *)-1; }
extern "C" int _getpid(void) { return 1; }
extern "C" int _kill(int, int) { return -1; }
extern "C" int _gettimeofday(struct timeval *tv, void *) {
	if (tv && _pax_svc) { int64_t us = _pax_svc->get_time_us(); tv->tv_sec = us / 1000000; tv->tv_usec = us % 1000000; }
	return 0;
}
extern "C" clock_t _times(struct tms *buf) {
	clock_t t = _pax_svc ? (clock_t)(_pax_svc->get_time_us() / 1000) : 0;
	if (buf) memset(buf, 0, sizeof(*buf));
	return t;
}
extern "C" char *getenv(const char *) { return NULL; }

static void scummvm_prepare_storage() {
	if (!_pax_svc || !_pax_svc->file_mkdir)
		return;
	/* file_mkdir intentionally mirrors POSIX mkdir (one component), so make
	 * the parent directories explicitly on the selected physical volume. */
	static const char *const suffixes[] = {
		"/scummvm",
		"/scummvm/saves",
		"/roms/scummvm"
	};
	for (const char *suffix : suffixes) {
		char dir[256];
		if (!scummvm_storage_path(dir, sizeof(dir), suffix)) continue;
		int result = _pax_svc->file_mkdir(dir);
		if (_pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM storage mkdir %s -> %d\n", dir, result);
	}
}

static bool scummvm_open_storage_file(const char *suffix, const char *mode,
		void **file_out, char *path_out, size_t path_out_size);

#ifdef SCUMMVM_PAPP2_NATIVE
/* P4 installs the ScummVM front ends with the emulator apps, but ScummVM's
 * own Add Game dialog does not index a ZIP as a game directory. Let the user
 * choose a ZIP, extract its complete tree to a per-archive cache, then ask
 * ScummVM to auto-detect the AGI/SCI game in that tree. */
static char s_scummvm_zip_game_path[256];

static bool scummvm_zip_ensure_dir(const char *path) {
	if (!path || !path[0]) return false;
	char dir[256];
	if (strlen(path) >= sizeof(dir)) return false;
	strcpy(dir, path);
	for (char *p = dir + 1; *p; ++p) {
		if (*p != '/') continue;
		*p = '\0';
		mkdir(dir, 0775);
		*p = '/';
	}
	mkdir(dir, 0775);
	struct stat st;
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool scummvm_zip_safe_member_path(const char *member) {
	if (!member || !member[0] || member[0] == '/' || member[0] == '\\') return false;
	if (strchr(member, ':') || strchr(member, '\\')) return false;
	size_t length = strlen(member);
	if (length && member[length - 1] == '/') --length; /* explicit ZIP directory entry */
	if (!length) return false;
	const char *part = member;
	for (const char *p = member; p <= member + length; ++p) {
		if (p == member + length || *p == '/') {
			size_t n = (size_t)(p - part);
			if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
				return false;
			if (p == member + length) break;
			part = p + 1;
		}
	}
	return true;
}

static bool scummvm_extract_game_zip(const char *archive_path, char *game_dir, size_t game_dir_size) {
	if (!archive_path || !game_dir || !game_dir_size) return false;
	POSIXFilesystemNode zip_node{Common::String(archive_path)};
	Common::SeekableReadStream *zip_stream = zip_node.createReadStream();
	if (!zip_stream) {
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM ZIP open failed: could not read %s\n", archive_path);
		return false;
	}
	Common::Archive *archive = Common::makeZipArchive(zip_stream);
	if (!archive) {
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM ZIP open failed: archive parser rejected %s\n", archive_path);
		return false;
	}
	Common::ArchiveMemberList members;
	int member_count = archive->listMembers(members);
	if (member_count <= 0) {
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM ZIP contains no readable members: %s\n", archive_path);
		delete archive;
		return false;
	}

	FILE *zip_file = fopen(archive_path, "rb");
	if (!zip_file || fseek(zip_file, 0, SEEK_END) != 0) {
		if (zip_file) fclose(zip_file);
		delete archive;
		return false;
	}
	long zip_size = ftell(zip_file);
	fclose(zip_file);
	if (zip_size < 0) { delete archive; return false; }
	uint32_t hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)archive_path; *p; ++p) {
		hash = (hash ^ *p) * 16777619u;
	}
	hash = (hash ^ (uint32_t)zip_size) * 16777619u;
	char cache_root[256];
	char cache_suffix[64];
	snprintf(cache_suffix, sizeof(cache_suffix), "/roms/scummvm/.papp_zipcache/%08lx",
		(unsigned long)hash);
	if (!scummvm_storage_path(cache_root, sizeof(cache_root), cache_suffix) ||
		!scummvm_zip_ensure_dir(cache_root)) {
		delete archive;
		return false;
	}

	const uint64_t max_total = 512ull * 1024ull * 1024ull;
	uint64_t total = 0;
	uint8_t buffer[8192];
	bool ok = true;
	for (Common::ArchiveMemberList::const_iterator it = members.begin(); it != members.end() && ok; ++it) {
		const Common::ArchiveMemberPtr &member = *it;
		const Common::Path member_path = member->getPathInArchive();
		Common::String relative = member_path.toString('/');
		if (!scummvm_zip_safe_member_path(relative.c_str())) {
			if (_pax_svc && _pax_svc->log_printf)
				_pax_svc->log_printf("ScummVM ZIP rejected unsafe member: %s\n", relative.c_str());
			ok = false;
			break;
		}
		char target[512];
		int n = snprintf(target, sizeof(target), "%s/%s", cache_root, relative.c_str());
		if (n < 0 || (size_t)n >= sizeof(target)) { ok = false; break; }
		/* Some ZIP writers encode directory entries only with a trailing slash,
		 * while ScummVM's archive layer also consults the external attributes.
		 * Treat either representation as a directory so we don't try to open
		 * directory records as regular files. */
		if (member->isDirectory() || member_path.isSeparatorTerminated() ||
			(!relative.empty() && relative.c_str()[relative.size() - 1] == '/')) {
			size_t target_len = strlen(target);
			while (target_len > 1 && target[target_len - 1] == '/')
				target[--target_len] = '\0';
			ok = scummvm_zip_ensure_dir(target);
			continue;
		}
		char *slash = strrchr(target, '/');
		if (!slash) { ok = false; break; }
		*slash = '\0';
		ok = scummvm_zip_ensure_dir(target);
		*slash = '/';
		if (!ok) break;
		Common::SeekableReadStream *input = member->createReadStream();
		FILE *output = fopen(target, "wb");
		if (!input || !output) {
			delete input;
			if (output) fclose(output);
			ok = false;
			break;
		}
		while (!input->eos() && !input->err()) {
			uint32_t read = input->read(buffer, sizeof(buffer));
			if (!read) {
				if (!input->eos()) ok = false;
				break;
			}
			total += read;
			if (total > max_total || fwrite(buffer, 1, read, output) != read) {
				ok = false;
				break;
			}
		}
		if (input->err() || ferror(output)) ok = false;
		delete input;
		if (fclose(output) != 0) ok = false;
	}
	delete archive;
	if (!ok || total == 0 || strlen(cache_root) + 1 > game_dir_size) {
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM ZIP extraction failed: %s total=%llu\n",
				archive_path, (unsigned long long)total);
		return false;
	}
	snprintf(game_dir, game_dir_size, "%s", cache_root);
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("ScummVM ZIP extracted: %s -> %s (%llu bytes)\n",
			archive_path, game_dir, (unsigned long long)total);
	return true;
}

/* Verify that the extracted tree is visible through the same POSIX filesystem
 * facade ScummVM uses for autodetection. ZIP extraction uses the PAPP file
 * service directly, so a successful write alone does not prove that
 * opendir/stat can see the resulting directory. */
static bool scummvm_zip_verify_tree(const char *game_dir) {
	struct stat dir_stat{};
	int stat_result = stat(game_dir, &dir_stat);
	DIR *dir = opendir(game_dir);
	int entries = 0;
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			if (entry->d_name[0] == '.') continue;
			if (_pax_svc && _pax_svc->log_printf)
				_pax_svc->log_printf("ScummVM ZIP visible entry: %s%s\n",
					entry->d_name, entry->d_type == DT_DIR ? "/" : "");
			++entries;
		}
		closedir(dir);
	}
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("ScummVM ZIP tree check: %s stat=%d dir=%d entries=%d\n",
			game_dir, stat_result, stat_result == 0 && S_ISDIR(dir_stat.st_mode), entries);
	return stat_result == 0 && S_ISDIR(dir_stat.st_mode) && entries > 0;
}

static bool scummvm_direct_launch_pending(void) {
	char path[256];
	void *file = NULL;
	if (!scummvm_open_storage_file("/roms/pax/scummvm-sci.rom", "rb", &file, path, sizeof(path)) || !file)
		return false;
	char byte;
	bool pending = _pax_svc->file_read(&byte, 1, 1, file) == 1;
	_pax_svc->file_close(file);
	return pending;
}

static int scummvm_choose_zip_start(void) {
	if (scummvm_direct_launch_pending()) return 0;
	scummvm_prepare_storage();
	char game_root[128];
	if (!scummvm_storage_path(game_root, sizeof(game_root), "/roms/scummvm")) return 0;
	static const char *const choices[] = { "Choose a ZIP game", "Open ScummVM game list", "Cancel" };
	for (;;) {
		int choice = papp_ui_menu("ScummVM", choices, 3, 0);
		if (choice == 1) return 0;
		if (choice != 0) return -1;
		papp_pick_cfg_t cfg = { .title = "ScummVM ZIP games", .root = game_root, .extensions = ".zip" };
		papp_pick_result_t picked = {};
		if (papp_ui_pick_file(&cfg, &picked) != PAPP_PICK_RUN) continue;
		if (scummvm_extract_game_zip(picked.path, s_scummvm_zip_game_path,
				sizeof(s_scummvm_zip_game_path))) return 1;
		papp_ui_message("ScummVM ZIP", "Could not extract this ZIP. Check that it is a supported, unencrypted ZIP archive.");
	}
}
#endif

static bool scummvm_open_storage_file(const char *suffix, const char *mode,
		void **file_out, char *path_out, size_t path_out_size) {
	if (!file_out || !path_out || !mode ||
		!scummvm_storage_path(path_out, path_out_size, suffix) ||
		!_pax_svc || !_pax_svc->file_open)
		return false;
	*file_out = _pax_svc->file_open(path_out, mode);
	return *file_out != NULL;
}

class PaxConfigReadStream final : public Common::SeekableReadStream {
public:
	explicit PaxConfigReadStream(const char *path)
		: _file(NULL), _size(0), _eos(false), _error(false) {
		if (!_pax_svc || !_pax_svc->file_open) return;
		_file = _pax_svc->file_open(path, "rb");
		if (!_file) return;
		if (_pax_svc->file_seek(_file, 0, SEEK_END) != 0) {
			_error = true;
			return;
		}
		_size = _pax_svc->file_tell(_file);
		if (_size < 0 || _pax_svc->file_seek(_file, 0, SEEK_SET) != 0) {
			_error = true;
			_size = 0;
		}
	}

	~PaxConfigReadStream() override {
		if (_file && _pax_svc && _pax_svc->file_close)
			_pax_svc->file_close(_file);
	}

	bool isOpen() const { return _file != NULL && !_error; }
	uint32 read(void *dataPtr, uint32 dataSize) override {
		if (!isOpen() || dataSize == 0) return 0;
		size_t got = _pax_svc->file_read(dataPtr, 1, dataSize, _file);
		if (got < dataSize) _eos = true;
		return (uint32)got;
	}
	bool eos() const override { return _eos; }
	bool err() const override { return _error; }
	void clearErr() override { _eos = false; _error = false; }
	int64 pos() const override {
		return isOpen() ? (int64)_pax_svc->file_tell(_file) : -1;
	}
	int64 size() const override { return _size; }
	bool seek(int64 offset, int whence = SEEK_SET) override {
		if (!isOpen() || offset < LONG_MIN || offset > LONG_MAX) {
			_error = true;
			return false;
		}
		if (_pax_svc->file_seek(_file, (long)offset, whence) != 0) {
			_error = true;
			return false;
		}
		_eos = false;
		return true;
	}

private:
	void *_file;
	int64 _size;
	bool _eos;
	bool _error;
};

class PaxConfigWriteStream final : public Common::MemoryWriteStreamDynamic {
public:
	PaxConfigWriteStream() : Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES) {}

	~PaxConfigWriteStream() override {
		if (!_pax_svc || !_pax_svc->file_open || !_data) return;
		scummvm_prepare_storage();
		char config_path[256] = "";
		void *file = NULL;
		bool opened = scummvm_open_storage_file("/roms/scummvm/scummvm.ini", "wb",
			&file, config_path, sizeof(config_path));
		if (!opened) {
			if (_pax_svc->log_printf)
				_pax_svc->log_printf("ScummVM config open failed: %s\n", config_path);
			return;
		}
		size_t written = _pax_svc->file_write(_data, 1, _size, file);
		_pax_svc->file_close(file);
		if (_pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM config saved: %u/%u bytes\n",
				(unsigned)written, (unsigned)_size);
	}
};

/* The launcher writes this short record when the user chooses:
 *   ScummVM PAX -> SCI32 Engine -> <game>
 *
 * Lines are: portable game path, engine id, game id, display description.
 * Keeping this outside scummvm.ini means the normal ScummVM game list and
 * key mappings remain persistent, while a dashboard launch is one-shot. */
static void scummvm_trim_line(char *line) {
	if (!line) return;
	line[strcspn(line, "\r\n")] = '\0';
}

static bool scummvm_load_direct_target() {
	char direct_path[256];
	void *direct_file = NULL;
	if (!scummvm_open_storage_file("/roms/pax/scummvm-sci.rom", "rb",
		&direct_file, direct_path, sizeof(direct_path)))
		return false;
	FILE *file = (FILE *)direct_file;
	if (!file) return false;

	char path[256] = "";
	char engine[32] = "";
	char game_id[64] = "";
	char description[96] = "";
	bool valid = fgets(path, sizeof(path), file) != NULL &&
	             fgets(engine, sizeof(engine), file) != NULL &&
	             fgets(game_id, sizeof(game_id), file) != NULL;
	if (valid) {
		scummvm_trim_line(path);
		scummvm_trim_line(engine);
		scummvm_trim_line(game_id);
		if (fgets(description, sizeof(description), file) != NULL)
			scummvm_trim_line(description);
	}
	fclose(file);

	if (!valid || !path[0] || !engine[0] || !game_id[0]) {
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM direct launch record is incomplete\n");
		return false;
	}

	const char *domain = "pax_direct";
	ConfMan.addGameDomain(domain);
	ConfMan.set("engineid", engine, domain);
	ConfMan.set("gameid", game_id, domain);
	ConfMan.setPath("path", Common::Path(path), domain);
	ConfMan.set("description", description[0] ? description : game_id, domain);
	ConfMan.setActiveDomain(domain);

	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("ScummVM direct target: engine=%s game=%s path=%s\n",
			engine, game_id, path);

	/* Consume the dashboard request.  Opening with wb is supported by the
	 * launcher's file service and lets the next ordinary PAX launch show the
	 * ScummVM launcher instead of replaying the previous game. */
	file = fopen(direct_path, "wb");
	if (file) fclose(file);
	return true;
}

/* ── PAX graphics manager ──────────────────────────────────────────── */

class PaxGraphicsManager final : public GraphicsManager {
public:
	PaxGraphicsManager() : _width(400), _height(240), _format(Graphics::PixelFormat::createFormatCLUT8()),
		_screen(), _overlay(), _cursor(), _cursorMask(NULL), _rgb(NULL), _rgbPixels(0),
		_overlayVisible(false), _cursorVisible(true), _cursorX(0), _cursorY(0),
		_cursorAreaW(400), _cursorAreaH(240), _cursorHotX(0), _cursorHotY(0),
		_cursorKeyColor(0), _lastUpdate(0) {
		memset(_palette, 0, sizeof(_palette));
		memset(_palette565, 0, sizeof(_palette565));
		memset(_cursorPalette, 0, sizeof(_cursorPalette));
	}
	~PaxGraphicsManager() override {
		_screen.free(); _overlay.free(); _cursor.free();
		if (_cursorMask && _pax_svc) _pax_svc->mem_free(_cursorMask);
		if (_rgb && _pax_svc) _pax_svc->mem_free(_rgb);
	}
	void init() {
		_overlay.create(400, 240, getOverlayFormat());
		memset(_overlay.getPixels(), 0, _overlay.pitch * _overlay.h);
	}
	bool hasFeature(OSystem::Feature f) const override {
		return f == OSystem::kFeatureTouchscreen || f == OSystem::kFeatureVirtualKeyboard || f == OSystem::kFeatureNoQuit;
	}
	void setFeatureState(OSystem::Feature, bool) override {}
	bool getFeatureState(OSystem::Feature f) const override { return hasFeature(f); }
	void initSize(uint width, uint height, const Graphics::PixelFormat *format = NULL) override {
		_screen.free();
		_width = width; _height = height;
		_format = format ? *format : Graphics::PixelFormat::createFormatCLUT8();
		_screen.create(width, height, _format);
		if (_rgb && _pax_svc) _pax_svc->mem_free(_rgb);
		/* Keep enough room for ScummVM's 400x240 RGB565 overlay as well as
		 * the game framebuffer.  The GUI overlay is larger than most game
		 * surfaces (for example SCI is commonly 320x200). */
		_rgbPixels = width * height;
		if (_rgbPixels < 400u * 240u) _rgbPixels = 400u * 240u;
		_rgb = (uint16_t *)_pax_svc->mem_caps_alloc(_rgbPixels * sizeof(uint16_t), PAX_MEM_CAP_SPIRAM | PAX_MEM_CAP_DMA);
		memset(_palette, 0, sizeof(_palette));
		memset(_palette565, 0, sizeof(_palette565));
		if (_rgb) memset(_rgb, 0, _rgbPixels * sizeof(uint16_t));
	}
	int getScreenChangeID() const override { return 0; }
	void beginGFXTransaction() override {}
	OSystem::TransactionError endGFXTransaction() override { return OSystem::kTransactionSuccess; }
	int16 getHeight() const override { return (int16)_height; }
	int16 getWidth() const override { return (int16)_width; }
#ifdef USE_RGB_COLOR
	Graphics::PixelFormat getScreenFormat() const override { return _format; }
	Common::List<Graphics::PixelFormat> getSupportedFormats() const override {
		Common::List<Graphics::PixelFormat> formats;
		formats.push_back(Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0));
		formats.push_back(Graphics::PixelFormat::createFormatCLUT8());
		return formats;
	}
#endif
	void setPalette(const byte *colors, uint start, uint num) override {
		if (!colors) return;
		if (start + num > 256) num = 256 - start;
		memcpy(_palette + start * 3, colors, num * 3);
		for (uint i = start; i < start + num; ++i) {
			const byte *color = _palette + i * 3;
			_palette565[i] = (uint16_t)(((color[0] >> 3) << 11) |
				((color[1] >> 2) << 5) | (color[2] >> 3));
		}
	}
	void grabPalette(byte *colors, uint start, uint num) const override {
		if (colors && start + num <= 256) memcpy(colors, _palette + start * 3, num * 3);
	}
	void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override { _screen.copyRectToSurface(buf, pitch, x, y, w, h); }
	Graphics::Surface *lockScreen() override { return &_screen; }
	void unlockScreen() override {}
	void updateScreen() override {
		if (!_pax_svc || !_pax_svc->display_write_frame_custom || !_rgb) return;
		int64_t now = _pax_svc->get_time_us();
		/* Match the P4 panel's 60 Hz refresh instead of dropping frames at
		 * 40 Hz. This keeps ScummVM's draw cadence from beating against the
		 * panel refresh and reduces uneven motion in both AGI and SCI games. */
		if (now - _lastUpdate < 16667) return;
		_lastUpdate = now;
		uint16_t *out = _rgb;
		uint16_t frame_w = (uint16_t)_width;
		uint16_t frame_h = (uint16_t)_height;
		if (_overlayVisible) {
			/* The overlay is a 400x240 Surface and may have padding in its
			 * pitch.  Repack it into the same tightly packed DMA buffer used
			 * by the game framebuffer, and pass its real dimensions to the
			 * display pipeline.  Treating it as _width x _height was the
			 * source of the chopped/green GUI frames. */
			const uint8_t *src = (const uint8_t *)_overlay.getPixels();
			uint8_t *dst = (uint8_t *)_rgb;
			frame_w = (uint16_t)_overlay.w;
			frame_h = (uint16_t)_overlay.h;
			for (uint y = 0; y < frame_h; ++y)
				memcpy(dst + y * frame_w * sizeof(uint16_t),
				       src + y * _overlay.pitch,
				       frame_w * sizeof(uint16_t));
		} else if (_format.bytesPerPixel == 2) {
			/* The display/PPA path requires a tightly packed RGB565 frame.
			 * Graphics::Surface may have a padded pitch, so repack each row
			 * instead of assuming width == pitch / bytes-per-pixel. */
			const uint8_t *src = (const uint8_t *)_screen.getPixels();
			uint8_t *dst = (uint8_t *)_rgb;
			for (uint y = 0; y < _height; ++y)
				memcpy(dst + y * _width * sizeof(uint16_t),
				       src + y * _screen.pitch,
				       _width * sizeof(uint16_t));
		} else {
			const uint8_t *src = (const uint8_t *)_screen.getPixels();
			for (uint y = 0; y < _height; ++y) {
				const uint8_t *src_row = src + y * _screen.pitch;
				uint16_t *dst_row = _rgb + y * _width;
				for (uint x = 0; x < _width; ++x)
					dst_row[x] = _palette565[src_row[x]];
			}
		}
		drawCursor(out, frame_w, frame_h);
		float scale = 1.0f;
		if (frame_w && frame_h) {
			float sx = 800.0f / (float)frame_w, sy = 480.0f / (float)frame_h;
			scale = sx < sy ? sx : sy;
			if (scale > 2.4f) scale = 2.4f;
			if (scale < 1.0f) scale = 1.0f;
		}
		_pax_svc->display_write_frame_custom(out, frame_w, frame_h, scale, false);
	}
	void setShakePos(int, int) override {}
	void fillScreen(uint32 col) override { if (_screen.getPixels()) _screen.fillRect(Common::Rect(_screen.w, _screen.h), col); }
	void fillScreen(const Common::Rect &r, uint32 col) override { if (_screen.getPixels()) _screen.fillRect(r, col); }
	void setFocusRectangle(const Common::Rect &) override {}
	void clearFocusRectangle() override {}
	void showOverlay(bool visible) override { _overlayVisible = visible; }
	void hideOverlay() override { _overlayVisible = false; }
	bool isOverlayVisible() const override { return _overlayVisible; }
	uint overlayInputWidth() const { return _overlayVisible ? _overlay.w : _width; }
	uint overlayInputHeight() const { return _overlayVisible ? _overlay.h : _height; }
	Graphics::PixelFormat getOverlayFormat() const override { return Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0); }
	void clearOverlay() override { memset(_overlay.getPixels(), 0, _overlay.pitch * _overlay.h); }
	void grabOverlay(Graphics::Surface &surface) const override { surface.copyFrom(_overlay); }
	void copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) override { _overlay.copyRectToSurface(buf, pitch, x, y, w, h); }
	int16 getOverlayHeight() const override { return 240; }
	int16 getOverlayWidth() const override { return 400; }
	bool showMouse(bool visible) override {
		bool previous = _cursorVisible;
		_cursorVisible = visible;
		return previous;
	}
	void warpMouse(int x, int y) override {
		_cursorX = x;
		_cursorY = y;
		_cursorAreaW = _overlayVisible ? _overlay.w : _width;
		_cursorAreaH = _overlayVisible ? _overlay.h : _height;
	}
	void setMousePosition(int x, int y, uint areaW, uint areaH) {
		_cursorX = x;
		_cursorY = y;
		_cursorAreaW = areaW ? areaW : _width;
		_cursorAreaH = areaH ? areaH : _height;
	}
	void setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
			uint32 keycolor, const Graphics::PixelFormat *format,
			const byte *mask, frac_t, frac_t) override {
		_cursor.free();
		if (_cursorMask && _pax_svc) {
			_pax_svc->mem_free(_cursorMask);
			_cursorMask = NULL;
		}
		_cursorHotX = hotspotX;
		_cursorHotY = hotspotY;
		_cursorKeyColor = keycolor;
		if (!buf || w == 0 || h == 0)
			return;

		Graphics::PixelFormat actualFormat = format ? *format : Graphics::PixelFormat::createFormatCLUT8();
		_cursor.create(w, h, actualFormat);
		_cursor.copyRectToSurface(buf, w * actualFormat.bytesPerPixel, 0, 0, w, h);
		if (mask) {
			size_t maskBytes = (size_t)w * h;
			_cursorMask = (byte *)_pax_svc->mem_caps_alloc(maskBytes, PAX_MEM_CAP_SPIRAM);
			if (_cursorMask)
				memcpy(_cursorMask, mask, maskBytes);
		}
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM cursor installed: %ux%u hotspot=(%d,%d) bpp=%u mask=%s\n",
				(unsigned)w, (unsigned)h, hotspotX, hotspotY,
				(unsigned)actualFormat.bytesPerPixel, mask ? "yes" : "no");
	}
	void setCursorPalette(const byte *colors, uint start, uint num) override {
		if (!colors || start >= 256) return;
		if (num > 256 - start) num = 256 - start;
		memcpy(_cursorPalette + start * 3, colors, num * 3);
	}
private:
	static uint16_t cursorRgb565(uint8 r, uint8 g, uint8 b) {
		return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
	}
	void drawCursor(uint16_t *frame, uint frameW, uint frameH) {
		if (!_cursorVisible || !frame || frameW == 0 || frameH == 0)
			return;

		int cx = _cursorAreaW ? (_cursorX * (int)frameW) / (int)_cursorAreaW : _cursorX;
		int cy = _cursorAreaH ? (_cursorY * (int)frameH) / (int)_cursorAreaH : _cursorY;
		int originX = cx - _cursorHotX;
		int originY = cy - _cursorHotY;
		if (_cursor.w > 0 && _cursor.h > 0 && _cursor.getPixels()) {
			const uint bpp = _cursor.format.bytesPerPixel;
			for (int y = 0; y < _cursor.h; ++y) {
				for (int x = 0; x < _cursor.w; ++x) {
					int dstX = originX + x;
					int dstY = originY + y;
					if (dstX < 0 || dstY < 0 || dstX >= (int)frameW || dstY >= (int)frameH)
						continue;
					size_t index = (size_t)y * _cursor.pitch + (size_t)x * bpp;
					const byte *pixel = (const byte *)_cursor.getPixels() + index;
					if (_cursorMask && _cursorMask[(size_t)y * _cursor.w + x] == kCursorMaskTransparent)
						continue;

					uint32 color = 0;
					if (bpp == 1) color = pixel[0];
					else if (bpp == 2) color = (uint32)pixel[0] | ((uint32)pixel[1] << 8);
					else if (bpp == 3) color = (uint32)pixel[0] | ((uint32)pixel[1] << 8) | ((uint32)pixel[2] << 16);
					else if (bpp >= 4) color = (uint32)pixel[0] | ((uint32)pixel[1] << 8) |
						((uint32)pixel[2] << 16) | ((uint32)pixel[3] << 24);

					uint8 a = 255, r = 255, g = 255, b = 255;
					if (_cursor.format == Graphics::PixelFormat::createFormatCLUT8()) {
						if (color == _cursorKeyColor) continue;
						r = _cursorPalette[color * 3 + 0];
						g = _cursorPalette[color * 3 + 1];
						b = _cursorPalette[color * 3 + 2];
					} else {
						if (color == _cursorKeyColor) continue;
						_cursor.format.colorToARGB(color, a, r, g, b);
						if (a == 0) continue;
					}
					frame[(size_t)dstY * frameW + dstX] = cursorRgb565(r, g, b);
				}
			}
			return;
		}

		/* Some engines do not install a cursor until their first input event.
		 * Keep a visible, high-contrast fallback so the ScummVM launcher remains
		 * usable on the first frame as well. */
		static const int shape[][2] = {
			{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7},
			{1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6},
			{2, 2}, {2, 3}, {2, 4}, {2, 5},
			{3, 3}, {3, 4}, {3, 5}, {4, 4}, {4, 5}, {5, 5}
		};
		for (size_t i = 0; i < sizeof(shape) / sizeof(shape[0]); ++i) {
			int x = cx + shape[i][0], y = cy + shape[i][1];
			if (x >= 0 && y >= 0 && x < (int)frameW && y < (int)frameH)
				frame[(size_t)y * frameW + x] = cursorRgb565(255, 255, 255);
		}
	}
	uint _width, _height;
	Graphics::PixelFormat _format;
	Graphics::Surface _screen, _overlay, _cursor;
	byte *_cursorMask;
	uint16_t *_rgb;
	uint32_t _rgbPixels;
	byte _palette[256 * 3];
	uint16_t _palette565[256];
	byte _cursorPalette[256 * 3];
	bool _overlayVisible;
	bool _cursorVisible;
	int _cursorX, _cursorY;
	uint _cursorAreaW, _cursorAreaH;
	int _cursorHotX, _cursorHotY;
	uint32 _cursorKeyColor;
	int64_t _lastUpdate;
};

/* ── PAX mixer manager ─────────────────────────────────────────────── */

class PaxMixerManager final : public MixerManager {
public:
	PaxMixerManager(int freq, int bufSize) : _freq(freq), _bufSize(bufSize), _buffer(NULL), _task(NULL), _stop(false) {}
	~PaxMixerManager() override {
		stopAudioTask();
		if (s_pax_mixer_manager == this) s_pax_mixer_manager = nullptr;
		if (_buffer && _pax_svc) _pax_svc->mem_free(_buffer);
	}
	void init() override {
		s_pax_mixer_manager = this;
		_mixer = new Audio::MixerImpl(_freq);
		if (_pax_svc && _pax_svc->audio_init) _pax_svc->audio_init(_freq);
		_buffer = (byte *)_pax_svc->mem_caps_alloc(_bufSize, PAX_MEM_CAP_SPIRAM);
		if (_buffer && _pax_svc) _pax_svc->task_create(audioTaskStub, "scvm_audio", 16384, this, 5, &_task, 1);
		_mixer->setReady(true);
	}
	void stopAudioTask() {
		__atomic_store_n(&_stop, true, __ATOMIC_RELEASE);
		if (_task && _pax_svc && _pax_svc->task_delete) {
			_pax_svc->task_delete(_task);
			_task = nullptr;
		}
	}
	void suspendAudio() override { _audioSuspended = true; }
	int resumeAudio() override { if (!_audioSuspended) return -2; _audioSuspended = false; return 0; }
private:
	static void audioTaskStub(void *arg) { ((PaxMixerManager *)arg)->audioTask(); }
	void audioTask() {
		while (!__atomic_load_n(&_stop, __ATOMIC_ACQUIRE)) {
			if (_audioSuspended) {
				if (_pax_svc) _pax_svc->delay_ms(10);
				continue;
			}
			if (!_mixer || !_buffer) break;
			_mixer->mixCallback(_buffer, _bufSize);
			if (_pax_svc && _pax_svc->audio_submit)
				_pax_svc->audio_submit((short *)_buffer, _bufSize / 4);
		}
	}
	int _freq, _bufSize;
	byte *_buffer;
	void *_task;
	volatile bool _stop;
};

static void scummvm_stop_audio_task() {
	if (s_pax_mixer_manager) s_pax_mixer_manager->stopAudioTask();
}

class PaxFilesystemFactory final : public FilesystemFactory {
protected:
	AbstractFSNode *makeRootFileNode() const override {
		char path[32];
		scummvm_storage_path(path, sizeof(path), "/");
		return new POSIXFilesystemNode(path);
	}
	AbstractFSNode *makeCurrentDirectoryFileNode() const override {
		char path[32];
		scummvm_storage_path(path, sizeof(path), "/");
		return new POSIXFilesystemNode(path);
	}
	AbstractFSNode *makeFileNodePath(const Common::String &path) const override { return new POSIXFilesystemNode(path); }
};

class PaxMutex final : public Common::MutexInternal {
public:
	bool lock() override { return true; }
	bool unlock() override { return true; }
};

/* ── ScummVM OSystem bridge ─────────────────────────────────────────── */

class OSystem_pax final : public ModularMixerBackend, public ModularGraphicsBackend, public Common::EventSource {
public:
	OSystem_pax() : _lastTouch(false), _lastMouseButtons(0), _mouseX(0), _mouseY(0), _head(0), _tail(0) {
		_fsFactory = new PaxFilesystemFactory();
		memset(_padPrev, 0, sizeof(_padPrev));
	}
	~OSystem_pax() override {}
	Common::HardwareInputSet *getHardwareInputSet() override {
		/* USE_NULL_DRIVER keeps ScummVM's desktop input backend out of the PAX,
		 * but the keymapper still needs a description of the inputs generated by
		 * this bridge. Without this, game-specific keymaps are never initialized
		 * and the engine reports "No hardware inputs were registered". The PAX
		 * converts the physical controller to ScummVM keyboard events, so the
		 * standard keyboard set is also the correct representation for controller
		 * actions. */
		Common::CompositeHardwareInputSet *inputs = new Common::CompositeHardwareInputSet();
		inputs->addHardwareInputSet(new Common::MouseHardwareInputSet(Common::defaultMouseButtons));
		inputs->addHardwareInputSet(new Common::KeyboardHardwareInputSet(Common::defaultKeys, Common::defaultModifiers));
		return inputs;
	}
	Common::SeekableReadStream *createConfigReadStream() override {
		char config_path[256];
		scummvm_storage_path(config_path, sizeof(config_path), "/roms/scummvm/scummvm.ini");
		PaxConfigReadStream *stream = new PaxConfigReadStream(config_path);
		if (!stream->isOpen()) {
			delete stream;
			return NULL;
		}
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM config loaded: %s\n", config_path);
		return stream;
	}
	Common::WriteStream *createConfigWriteStream() override {
		return new PaxConfigWriteStream();
	}
	void initBackend() override {
		if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM OSystem init begin\n");
		scummvm_prepare_storage();
		_timerManager = new DefaultTimerManager();
		_eventManager = new DefaultEventManager(this);
		_savefileManager = new DefaultSaveFileManager();
		PaxGraphicsManager *graphics = new PaxGraphicsManager();
		graphics->init();
		_graphicsManager = graphics;
		_mixerManager = new PaxMixerManager(44100, 4096);
		_mixerManager->init();
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/scummvm/extras/");
		ConfMan.registerDefault("extrapath", Common::Path(path));
		/* ScummVM's built-in virtual keyboard loads its pack from vkeybdpath.
		 * Keep that pack beside the other shared ScummVM data on the SD card. */
		scummvm_storage_path(path, sizeof(path), "/scummvm/");
		ConfMan.registerDefault("vkeybdpath", Common::Path(path));
		/* The compact PAX runtime bundle keeps the common ScummVM archives
		 * directly in the selected ScummVM data directory rather than duplicating them in per-type
		 * subdirectories. */
		scummvm_storage_path(path, sizeof(path), "/scummvm/");
		ConfMan.registerDefault("iconspath", Common::Path(path));
		scummvm_storage_path(path, sizeof(path), "/scummvm/plugins/");
		ConfMan.registerDefault("pluginspath", Common::Path(path));
		scummvm_storage_path(path, sizeof(path), "/scummvm/saves/");
		ConfMan.registerDefault("savepath", Common::Path(path));
		scummvm_storage_path(path, sizeof(path), "/scummvm/");
		ConfMan.registerDefault("themepath", Common::Path(path));
		BaseBackend::initBackend();
		/* Existing configs may contain paths from the other volume. Rebase only
		 * the built-in storage paths; user paths elsewhere remain untouched. */
		scummvm_rebase_config_paths();
		if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM OSystem init complete\n");
	}
	bool pollEvent(Common::Event &event) override {
        if (_pax_svc && _pax_svc->input_l3_read && _pax_svc->input_l3_read()) {
            /* This is the loader's close control. Exit on the engine task,
             * rather than queuing repeated QUITs or waiting for the GUI's
             * platform quit policy. The existing entry checkpoint cleans up
             * OSystem and the tracked audio task before returning. */
            _pax_svc->log_printf("ScummVM: loader close observed\n");
            app_return_to_launcher();
        }
		((DefaultTimerManager *)getTimerManager())->checkTimers();
		if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		if (_pax_svc && _pax_svc->input_gamepad_read) {
			pax_gamepad_state_t pad; memset(&pad, 0, sizeof(pad));
			/* The currently shipped loader exposes the combined controller view. */
			_pax_svc->input_gamepad_read(&pad);
			for (int i = 0; i < PAX_INPUT_MAX; ++i) {
				if (pad.values[i] == _padPrev[i]) continue;
				int key = 0, ascii = 0;
				switch (i) {
				case PAX_INPUT_UP: key = Common::KEYCODE_UP; break;
				case PAX_INPUT_DOWN: key = Common::KEYCODE_DOWN; break;
				case PAX_INPUT_LEFT: key = Common::KEYCODE_LEFT; break;
				case PAX_INPUT_RIGHT: key = Common::KEYCODE_RIGHT; break;
				case PAX_INPUT_A: key = Common::KEYCODE_RETURN; ascii = Common::ASCII_RETURN; break;
				case PAX_INPUT_B: key = Common::KEYCODE_ESCAPE; ascii = Common::ASCII_ESCAPE; break;
				case PAX_INPUT_START: key = Common::KEYCODE_RETURN; ascii = Common::ASCII_RETURN; break;
				case PAX_INPUT_SELECT:
					if (pad.values[i]) {
						Common::Event toggleKeyboard;
						toggleKeyboard.type = Common::EVENT_VIRTUAL_KEYBOARD;
						queue(toggleKeyboard);
					}
					break;
				default: break;
				}
				if (key) {
					if (_pax_svc && _pax_svc->log_printf)
						_pax_svc->log_printf("ScummVM gamepad event input=%d key=%d ascii=%d down=%d\n",
							i, key, ascii, pad.values[i] != 0 ? 1 : 0);
					queueKey(key, ascii, pad.values[i] != 0);
				}
				_padPrev[i] = pad.values[i];
			}
			if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		}
		if (_pax_svc && _pax_svc->input_keyboard_read) {
			pax_keyboard_event_t kev;
			if (_pax_svc->input_keyboard_read(&kev)) {
				if (_pax_svc->log_printf)
					_pax_svc->log_printf("ScummVM keyboard event key=%d down=%d\n", kev.key, kev.down ? 1 : 0);
				queueKeyboard(kev);
				if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
			}
		}
		if (_pax_svc && _pax_svc->touch_read) {
			const int oldMouseX = _mouseX, oldMouseY = _mouseY;
			int x = 0, y = 0; int touched = _pax_svc->touch_read(&x, &y);
			/* Touch coordinates are reported in the fixed 800x480 PAX canvas.
			 * ScummVM keeps the game surface at (for example) 320x200, but
			 * its launcher/settings UI is drawn in the separate 400x240
			 * overlay. Use the active surface dimensions for hit testing. */
			uint touch_w = getWidth(), touch_h = getHeight();
			if (_graphicsManager) {
				PaxGraphicsManager *graphics = static_cast<PaxGraphicsManager *>(_graphicsManager);
				touch_w = graphics->overlayInputWidth();
				touch_h = graphics->overlayInputHeight();
			}
			if (touched) {
				/* touch_read() is part of the PAPP2 ABI and already returns the
				 * canonical source-canvas coordinate. The loader performs the
				 * physical-panel -> source-frame 180-degree conversion before
				 * entering the app. The display writer rotates the rendered
				 * canvas only when presenting it, so rotating here again would
				 * make every in-game touch land at the opposite point. */
				bool overlay = _graphicsManager &&
					static_cast<PaxGraphicsManager *>(_graphicsManager)->isOverlayVisible();
				(void)overlay;
				int event_x = x;
				int event_y = y;
				_mouseX = (int16)(event_x * touch_w / 800); _mouseY = (int16)(event_y * touch_h / 480);
				if (!_lastTouch && _pax_svc->log_printf) {
					_pax_svc->log_printf("ScummVM touch down service=(%d,%d) event=(%d,%d) surface=%ux%u overlay=%d\n",
						x, y, _mouseX, _mouseY, (unsigned)touch_w,
						(unsigned)touch_h, overlay ? 1 : 0);
				}
			} else if (_lastTouch && _pax_svc->log_printf) {
				_pax_svc->log_printf("ScummVM touch up event=(%d,%d)\n", _mouseX, _mouseY);
			}
			if (touched) {
				Common::Event move; move.type = Common::EVENT_MOUSEMOVE; move.mouse = Common::Point(_mouseX, _mouseY);
				if (!_lastTouch || oldMouseX != _mouseX || oldMouseY != _mouseY) queue(move);
				if (_graphicsManager)
					static_cast<PaxGraphicsManager *>(_graphicsManager)->setMousePosition(
						_mouseX, _mouseY, touch_w, touch_h);
				if (!_lastTouch) { Common::Event down = move; down.type = Common::EVENT_LBUTTONDOWN; queue(down); }
			} else if (_lastTouch) { Common::Event up; up.type = Common::EVENT_LBUTTONUP; up.mouse = Common::Point(_mouseX, _mouseY); queue(up); }
			_lastTouch = touched != 0;
			if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		}
		if (_pax_svc && _pax_svc->input_mouse_read) {
			int dx = 0, dy = 0, buttons = 0;
			if (_pax_svc->input_mouse_read(&dx, &dy, &buttons)) {
				uint mouse_w = getWidth(), mouse_h = getHeight();
				if (_graphicsManager) {
					PaxGraphicsManager *graphics = static_cast<PaxGraphicsManager *>(_graphicsManager);
					mouse_w = graphics->overlayInputWidth();
					mouse_h = graphics->overlayInputHeight();
				}
				_mouseX += dx; _mouseY += dy;
				if (_mouseX < 0) _mouseX = 0; if (_mouseY < 0) _mouseY = 0;
				if (_mouseX >= (int)mouse_w) _mouseX = (int)mouse_w - 1; if (_mouseY >= (int)mouse_h) _mouseY = (int)mouse_h - 1;
				if (_graphicsManager)
					static_cast<PaxGraphicsManager *>(_graphicsManager)->setMousePosition(
						_mouseX, _mouseY, mouse_w, mouse_h);
				Common::Event move; move.type = Common::EVENT_MOUSEMOVE; move.mouse = Common::Point(_mouseX, _mouseY);
				/* A connected mouse returns a sample even when stationary.
				 * Reporting that as an event prevents the dispatcher draining. */
				if (dx || dy) queue(move);
				if ((buttons & 1) != (_lastMouseButtons & 1)) { Common::Event b = move; b.type = (buttons & 1) ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP; queue(b); }
				if ((buttons & 2) != (_lastMouseButtons & 2)) { Common::Event b = move; b.type = (buttons & 2) ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP; queue(b); }
				_lastMouseButtons = buttons;
			}
			if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		}
		event.type = Common::EVENT_INVALID;
		return false;
	}
	Common::MutexInternal *createMutex() override { return new PaxMutex(); }
	uint32 getMillis(bool = false) override { return _pax_svc ? (uint32)(_pax_svc->get_time_us() / 1000) : 0; }
	void delayMillis(uint ms) override { if (_pax_svc) _pax_svc->delay_ms((int)ms); }
	void getTimeAndDate(TimeDate &td, bool = false) const override { memset(&td, 0, sizeof(td)); td.tm_mday = 1; td.tm_mon = 1; td.tm_year = 120; }
	void quit() override { app_return_to_launcher(); }
	void logMessage(LogMessageType::Type, const char *message) override { if (_pax_svc && _pax_svc->log_printf && message) _pax_svc->log_printf("%s", message); }
	void addSysArchivesToSearchSet(Common::SearchSet &s, int priority) override {
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/scummvm/");
		s.add("engine-data", new Common::FSDirectory(path, 4), priority);
		s.add("gui/themes", new Common::FSDirectory(path, 4), priority);
	}
	protected:
	Common::Path getDefaultConfigFileName() override {
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/roms/scummvm/scummvm.ini");
		return Common::Path(path);
	}
	Common::Path getDefaultLogFileName() override {
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/scummvm/scummvm.log");
		return Common::Path(path);
	}
private:
	void queue(const Common::Event &event) { int next = (_tail + 1) % 16; if (next != _head) { _events[_tail] = event; _tail = next; } }
	void queueKey(int key, int ascii, bool down) { Common::Event event; event.type = down ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP; event.kbd.keycode = (Common::KeyCode)key; event.kbd.ascii = ascii; queue(event); }
	void queueKeyboard(const pax_keyboard_event_t &in) {
		int key = in.key, ascii = 0;
		if (key >= 273 && key <= 276) {
			static const int arrows[] = { Common::KEYCODE_UP, Common::KEYCODE_DOWN, Common::KEYCODE_RIGHT, Common::KEYCODE_LEFT };
			key = arrows[key - 273];
		} else if (key == 13) { key = Common::KEYCODE_RETURN; ascii = Common::ASCII_RETURN; }
		else if (key == 27) { key = Common::KEYCODE_ESCAPE; ascii = Common::ASCII_ESCAPE; }
		else if (key == 8) key = Common::KEYCODE_BACKSPACE;
		else if (key == 9) key = Common::KEYCODE_TAB;
		else if (key >= 'a' && key <= 'z') {
			/* ScummVM text widgets use the ASCII field for printable text.
			 * The HID decoder already gives us lowercase ASCII for an
			 * unshifted key, so preserve it before translating the keycode. */
			ascii = key;
			key = Common::KEYCODE_a + key - 'a';
		}
		else if (key >= 'A' && key <= 'Z') { ascii = key; key = Common::KEYCODE_a + key - 'A'; }
		else ascii = (key >= 32 && key < 127) ? key : 0;
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM translated key=%d ascii=%d down=%d\n", key, ascii, in.down ? 1 : 0);
		queueKey(key, ascii, in.down != 0);
	}
	bool _lastTouch;
	int _lastMouseButtons;
	int16 _mouseX, _mouseY;
	int _padPrev[PAX_INPUT_MAX];
	Common::Event _events[16];
	int _head, _tail;
};

extern "C" OSystem *OSystem_esp32_create(bool) { return new OSystem_pax(); }

static int scummvm_run(const app_services_t *svc) {
	_pax_svc = svc;
	s_exit_requested = 0;
	/* Resolve the storage volume before ScummVM loads its config or creates
	 * any filesystem nodes. */
	scummvm_storage_root();
	/* The normal ESP32 ScummVM frontend assigns this global in app_main().
	 * PAXs enter through the launcher instead, so do it here before
	 * scummvm_main() checks that the backend exists. */
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("ScummVM PAX starting; constructing OSystem\n");
	g_system = new OSystem_pax();
	if (!g_system) {
		if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM OSystem allocation failed\n");
		return -1;
	}
	if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM OSystem constructed\n");
	if (setjmp(s_exit_env) == 0) {
		if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM entering scummvm_main\n");
		/* USE_NULL_DRIVER intentionally skips ScummVM's desktop config-file
		 * branch. Load the same VFS-backed config here, then optionally install
		 * the one-shot target selected by the dashboard before scummvm_main()
		 * decides whether to show its launcher dialog. */
		bool config_loaded = ConfMan.loadDefaultConfigFile(Common::Path());
		if (_pax_svc && _pax_svc->log_printf)
			_pax_svc->log_printf("ScummVM config load: %s\n",
				config_loaded ? "ok" : "missing or invalid");
		bool direct_target = scummvm_load_direct_target();
	#if defined(SCUMMVM_PAX_AUTOSTART_KQ6)
		/* Test build: bypass the launcher and auto-detect the installed KQ6
		 * directory so the engine/data/input path can be verified end-to-end. */
		char autostart_path[256];
		scummvm_storage_path(autostart_path, sizeof(autostart_path), "/roms/scummvm/kq6");
		char path_option[272];
		snprintf(path_option, sizeof(path_option), "--path=%s", autostart_path);
		const char *argv[] = { "scummvm", path_option, "--auto-detect", NULL };
		scummvm_main(3, argv);
	#elif defined(SCUMMVM_PAX_AUTOSTART_QFG4)
		/* Test build: bypass the launcher and auto-detect the installed QFG4
		 * directory so the engine/data/input path can be verified end-to-end. */
		char autostart_path[256];
		scummvm_storage_path(autostart_path, sizeof(autostart_path), "/roms/scummvm/qfg4");
		char path_option[272];
		snprintf(path_option, sizeof(path_option), "--path=%s", autostart_path);
		const char *argv[] = { "scummvm", path_option, "--auto-detect", NULL };
		scummvm_main(3, argv);
	#else
	#ifdef SCUMMVM_PAPP2_NATIVE
		if (!direct_target && s_scummvm_zip_game_path[0]) {
			char path_option[272];
			snprintf(path_option, sizeof(path_option), "--path=%s", s_scummvm_zip_game_path);
			const char *argv[] = { "scummvm", path_option, "--auto-detect", NULL };
			if (_pax_svc && _pax_svc->log_printf)
				_pax_svc->log_printf("ScummVM auto-detect ZIP game from %s\n", s_scummvm_zip_game_path);
			scummvm_main(3, argv);
		} else {
			const char *argv[] = { "scummvm", NULL };
			scummvm_main(1, argv);
		}
	#else
		const char *argv[] = { "scummvm", NULL };
		scummvm_main(1, argv);
	#endif
	#endif
		if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM scummvm_main returned\n");
	}
	if (g_system) {
        _pax_svc->log_printf("ScummVM: destroying OSystem\n");
		OSystem *system = g_system;
		g_system = NULL;
		system->destroy();
        _pax_svc->log_printf("ScummVM: OSystem destroyed\n");
	}
	if (_pax_svc && _pax_svc->display_clear) {
		_pax_svc->display_clear(0);
		_pax_svc->display_flush();
	}
	if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("ScummVM PAX exited (%d)\n", s_exit_requested);
	return 0;
}

/* The loader entry stack is intentionally small and internal. ScummVM's
 * processSettings alone uses more than 10 KiB, and engines need further
 * headroom. Run the engine on a tracked PSRAM task, like the Quake II port. */
static volatile bool scummvm_done;
static int scummvm_result;
static void scummvm_task(void *) {
    scummvm_result = scummvm_run(_pax_svc);
    scummvm_done = true;
#ifdef SCUMMVM_PAPP2_NATIVE
    return;
#else
    for (;;) _pax_svc->delay_ms(1000);
#endif
}
#ifdef SCUMMVM_PAPP2_NATIVE
static int scummvm_native_engine_task(void *arg) {
    scummvm_task(arg);
    return scummvm_result;
}
#ifndef SCUMMVM_PAPP2_ID
#error SCUMMVM_PAPP2_ID must be defined for a native PAPP2 build
#endif
#ifndef SCUMMVM_PAPP2_NAME
#error SCUMMVM_PAPP2_NAME must be defined for a native PAPP2 build
#endif
extern "C" {
__attribute__((section(".papp_manifest"), used))
extern const papp_manifest_t __papp_manifest = {
    .magic = PAPP_MANIFEST_MAGIC,
    .sdk_version = PAPP_SDK_VERSION,
    .id = SCUMMVM_PAPP2_ID,
    .name = SCUMMVM_PAPP2_NAME,
    .version = PAPP_APP_VERSION,
    .main_stack = 32768,
};
}

int main(int, char **) {
    static ScummVMPlatformApi api;
    scummvm_papp2_api_init(&api);
    _pax_svc = &api;
    int start_mode = scummvm_choose_zip_start();
    if (start_mode < 0) {
        scummvm_papp2_api_shutdown();
        return 0;
    }
    if (start_mode > 0 && !scummvm_zip_verify_tree(s_scummvm_zip_game_path)) {
        papp_ui_message("ScummVM ZIP", "The extracted game folder is not readable. Update P4 Launcher and try again.");
        scummvm_papp2_api_shutdown();
        return -1;
    }
    scummvm_done = false;
    scummvm_result = -1;
    const papp_thread_cfg_t cfg = {
        .name = "scummvm",
        .stack_size = 131072,
        .prio = PAPP_PRIO_HIGH,
        .core = -1,
        .flags = PAPP_THREAD_STACK_PSRAM,
    };
    papp_thread_t *task = papp_thread_create(scummvm_native_engine_task, NULL, &cfg);
    if (!task) {
        papp_log("ScummVM: engine thread creation failed\n");
        return -1;
    }
    while (!scummvm_done && !papp_exit_requested())
        papp_delay_ms(100);
    int exit_code = 0;
    papp_thread_join(task, PAPP_WAIT_FOREVER, &exit_code);
    scummvm_papp2_api_shutdown();
    return scummvm_result;
}
#else
extern "C" int app_entry(const app_services_t *svc) {
    _pax_svc = svc;
    scummvm_done = false;
    scummvm_result = -1;
    void *task = nullptr;
    if (!svc->task_create || svc->task_create(scummvm_task, "scummvm", 131072,
            nullptr, 5, &task, 0) != 0) {
        svc->log_printf("ScummVM: engine task creation failed\n");
        return -1;
    }
    while (!scummvm_done) svc->delay_ms(100);
    svc->task_delete(task);
    return scummvm_result;
}
#endif

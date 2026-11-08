/*
 * PAPP ABI v2 (preview) — shared between the launcher and PAPP applications.
 *
 * This header is the single source of truth for:
 *   - the .papp v2 container (header, relocation records)
 *   - the in-image import table and manifest
 *   - the service interfaces exported by the launcher
 *
 * It must stay free of ESP-IDF includes: apps build against it with only the
 * bare riscv32-esp-elf toolchain. See docs/ARCHITECTURE.md §5–6.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Container ──────────────────────────────────────────────────────────── */

#define PAPP2_MAGIC           0x32504150u   /* "PAP2" little-endian */
#define PAPP2_FMT_VERSION     1
#define PAPP2_HEADER_SIZE     64

/* File layout:
 *   [papp2_header_t][SLOW image: image_size bytes][FAST image: fast_image_size bytes]
 *   [relocs: relocs_count x papp2_reloc_t]
 * SLOW (PSRAM, executable alias) holds everything by default; .bss (bss_size)
 * follows it and is zeroed by the loader. FAST (internal SRAM, optional) holds
 * what the app tagged PAPP_FAST / PAPP_FAST_DATA, followed by fast_bss_size
 * zeroed bytes. A launcher without executable SRAM loads FAST into PSRAM.
 * All *_off fields are byte offsets from the start of the SLOW image. */
typedef struct __attribute__((packed)) {
    uint32_t magic;            /* PAPP2_MAGIC                                    */
    uint16_t fmt_version;      /* PAPP2_FMT_VERSION                              */
    uint16_t header_size;      /* PAPP2_HEADER_SIZE                              */
    uint32_t flags;            /* reserved, 0                                    */
    uint32_t image_size;       /* bytes of image stored in the file              */
    uint32_t bss_size;         /* zero-filled bytes following the image          */
    uint32_t entry_off;        /* _papp_start                                    */
    uint32_t imports_off;      /* papp_import_t[imports_count]                   */
    uint32_t imports_count;
    uint32_t manifest_off;     /* papp_manifest_t                                */
    uint32_t relocs_count;
    uint32_t image_crc32;      /* CRC32 (IEEE) of SLOW then FAST image bytes     */
    uint32_t relocs_crc32;     /* CRC32 of the relocation records                */
    uint32_t fast_image_size;  /* FAST region bytes stored in the file (or 0)    */
    uint32_t fast_bss_size;    /* zero-filled bytes following the FAST image     */
    uint32_t reserved[2];
} papp2_header_t;

_Static_assert(sizeof(papp2_header_t) == PAPP2_HEADER_SIZE, "papp2_header_t must be 64 bytes");

/* Relocation record. The site is at `offset` in the region named by the
 * PAPP2_RELOC_SITE_FAST bit; `value` is the link-time target (S + A) as an
 * offset into the region named by PAPP2_RELOC_VAL_FAST.
 *   ABS32/HI20/LO12_*: the site gets (region base + value).
 *   PCREL_HI20: an auipc whose target is in the *other* region (references
 *     within a region are pc-relative and need nothing); the loader stores
 *     delta = target - site and patches %pcrel_hi(delta).
 *   PCREL_LO12_*: the paired low part; `value` names the auipc's site (same
 *     encoding as off_type's offset + region bit) whose delta it uses. The
 *     packer emits each PCREL_HI20 before its PCREL_LO12 records.
 *   DIFF32: a 32-bit label difference (switch jump table) between labels in
 *     different regions; value 1 = FAST label minus SLOW label, 0xFFFFFFFF =
 *     the reverse. The loader adds (or subtracts) the extra distance the
 *     regions moved apart: (fast_base - 0x10000000) - slow_base. */
enum {
    PAPP2_R_ABS32        = 1,   /* 32-bit absolute word                        */
    PAPP2_R_HI20         = 2,   /* lui rd, %hi(value)                          */
    PAPP2_R_LO12_I       = 3,   /* I-type %lo(value) (addi / loads / jalr)     */
    PAPP2_R_LO12_S       = 4,   /* S-type %lo(value) (stores)                  */
    PAPP2_R_PCREL_HI20   = 5,   /* auipc crossing SLOW <-> FAST                */
    PAPP2_R_PCREL_LO12_I = 6,
    PAPP2_R_PCREL_LO12_S = 7,
    PAPP2_R_DIFF32       = 8,   /* label difference across regions             */
};

#define PAPP2_FAST_LINK_BASE 0x10000000u   /* papp.ld: link address of the FAST region */

typedef struct __attribute__((packed)) {
    uint32_t off_type;    /* bits 0..26 offset, 27 site in FAST, 28..31 type */
    uint32_t value;       /* bit 31: target in FAST                          */
} papp2_reloc_t;

#define PAPP2_RELOC_SITE_FAST (1u << 27)
#define PAPP2_RELOC_VAL_FAST  (1u << 31)
#define PAPP2_RELOC_OFF(r)    ((r)->off_type & 0x07FFFFFFu)
#define PAPP2_RELOC_TYPE(r)   ((r)->off_type >> 28)

/* ── Interfaces ─────────────────────────────────────────────────────────── */

#define PAPP_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Every interface starts with this header. `size` = sizeof the launcher's
 * struct, so callers can tell whether an appended function exists. */
typedef struct {
    uint32_t id;
    uint16_t version;
    uint16_t size;
} papp_iface_hdr_t;

/* Import table entry, placed by the SDK in section .papp_imports. */
enum {
    PAPP_IMPORT_REQUIRED = 0,
    PAPP_IMPORT_OPTIONAL = 1,
};

typedef struct {
    uint32_t id;
    uint16_t min_version;
    uint16_t flags;
    const void *iface;     /* filled by the loader; NULL if optional and absent */
} papp_import_t;

/* ── Manifest ───────────────────────────────────────────────────────────── */

#define PAPP_MANIFEST_MAGIC  0x464E414Du   /* "MANF" */

typedef struct {
    uint32_t magic;           /* PAPP_MANIFEST_MAGIC                         */
    uint32_t sdk_version;     /* PAPP_SDK_VERSION the app was built with      */
    char     id[32];          /* "demo.hello"                                */
    char     name[32];        /* "Hello"                                     */
    char     version[16];     /* "1.0.0"                                     */
    uint32_t main_stack;      /* bytes; 0 = default                          */
    uint32_t flags;           /* PAPP_MF_*                                   */
    uint32_t reserved[8];
} papp_manifest_t;

#define PAPP_MF_STACK_PSRAM  (1u << 0)   /* allocate the main stack in PSRAM */
#define PAPP_MF_HIDDEN       (1u << 1)   /* test / developer app: off the home screen unless
                                           "Show developer apps" is on */

/* Exit code (main's return value or papp_exit()) asking the App Manager to start
 * the same app again, fresh and without arguments: an emulator's "Exit game"
 * goes back to its ROM browser with clean core state. */
#define PAPP_EXIT_RELAUNCH   0x52454C41   /* 'RELA' */

/* ── SYS interface ──────────────────────────────────────────────────────── */

#define PAPP_IF_SYS       PAPP_FOURCC('S', 'Y', 'S', ' ')
#define PAPP_IF_SYS_VER   3      /* v2: local_time; v3: utc_time */

typedef struct {
    int16_t year;
    uint8_t month, day, hour, minute, second;
    uint8_t weekday;             /* 0 = Sunday */
} papp_datetime_t;

typedef struct {
    papp_iface_hdr_t hdr;
    uint32_t abi_version;                         /* launcher ABI (major << 16 | minor) */
    void     (*log_write)(const char *s, size_t len);
    int      (*log_vprintf)(const char *fmt, va_list ap);
    int64_t  (*time_us)(void);
    void     (*delay_ms)(uint32_t ms);
    bool     (*exit_requested)(void);             /* set by DevLink STOP / overlay quit */
    const char *(*board_id)(void);
    bool     (*local_time)(papp_datetime_t *out);   /* v2: false if the clock isn't set */
    bool     (*utc_time)(int64_t *seconds);          /* v3: Unix time (UTC); false if the clock isn't set */
} papp_if_sys_t;

/* ── MEM interface ──────────────────────────────────────────────────────── */

#define PAPP_IF_MEM       PAPP_FOURCC('M', 'E', 'M', ' ')
#define PAPP_IF_MEM_VER   1

/* Abstract capabilities — mapped by the launcher, never ESP-IDF values. */
#define PAPP_MEM_ANY      0u
#define PAPP_MEM_FAST     (1u << 0)   /* internal SRAM                       */
#define PAPP_MEM_DMA      (1u << 1)   /* usable by DMA / PPA (64 B aligned)  */
#define PAPP_MEM_LARGE    (1u << 2)   /* PSRAM                               */

typedef struct {
    papp_iface_hdr_t hdr;
    void *(*alloc)(size_t size, uint32_t caps, size_t align);
    void *(*realloc)(void *ptr, size_t size);
    void  (*free)(void *ptr);
    size_t (*free_size)(uint32_t caps);
} papp_if_mem_t;

/* ── FS interface ───────────────────────────────────────────────────────── */

#define PAPP_IF_FS        PAPP_FOURCC('F', 'S', ' ', ' ')
#define PAPP_IF_FS_VER    1

typedef struct papp_file papp_file_t;

typedef struct {
    papp_iface_hdr_t hdr;
    papp_file_t *(*open)(const char *path, const char *mode);
    int     (*close)(papp_file_t *f);
    size_t  (*read)(papp_file_t *f, void *buf, size_t len);
    size_t  (*write)(papp_file_t *f, const void *buf, size_t len);
    int     (*seek)(papp_file_t *f, long off, int whence);
    long    (*tell)(papp_file_t *f);
    long    (*size)(const char *path);            /* -1 if missing */
    int     (*remove)(const char *path);
    int     (*mkdir)(const char *path);
} papp_if_fs_t;

/* ── DISP interface ─────────────────────────────────────────────────────── */

#define PAPP_IF_DISP      PAPP_FOURCC('D', 'I', 'S', 'P')
#define PAPP_IF_DISP_VER  3      /* v2: touch_read(NULL) = display coordinates; v3: present_ext */

enum { PAPP_PIXFMT_RGB565 = 1 };

enum {
    PAPP_SCALE_FIT_ASPECT = 0,   /* largest size that keeps the aspect ratio */
    PAPP_SCALE_INTEGER    = 1,   /* largest whole-number factor              */
    PAPP_SCALE_STRETCH    = 2,   /* fill the display                         */
    PAPP_SCALE_NONE       = 3,   /* 1:1, centred                             */
};

typedef struct {
    uint16_t width, height;      /* landscape, as the app sees it            */
    uint16_t refresh_hz;
    uint16_t reserved;
} papp_disp_info_t;

typedef struct {
    uint16_t width, height;
    uint16_t format;             /* PAPP_PIXFMT_*                            */
    uint8_t  scale;              /* PAPP_SCALE_*                             */
    uint8_t  buffers;            /* 1: content persists between frames;
                                    2: lock() alternates, draw the whole frame */
    uint32_t reserved[4];
} papp_surface_cfg_t;

typedef struct {
    uint16_t x, y;               /* surface coordinates                      */
    uint16_t size;
    uint8_t  id;
    uint8_t  reserved;
} papp_touch_point_t;

typedef struct papp_surface papp_surface_t;

typedef struct {
    papp_iface_hdr_t hdr;
    void            (*info)(papp_disp_info_t *out);
    papp_surface_t *(*surface_create)(const papp_surface_cfg_t *cfg);   /* NULL on failure */
    void            (*surface_destroy)(papp_surface_t *s);
    void           *(*lock)(papp_surface_t *s, int *stride_px);         /* buffer to draw into */
    int             (*present)(papp_surface_t *s, uint32_t flags);      /* waits for vsync; 0 = ok */
    /* Points in s's coordinates (outside it: dropped). v2: s == NULL gives
     * landscape display coordinates, e.g. for touch zones beside the picture. */
    int             (*touch_read)(papp_surface_t *s, papp_touch_point_t *pts, int max);
    /* v3: present the app's own buffer without copying it (width x height RGB565,
     * stride = width); like present, returns once the frame is in the back buffer. */
    int             (*present_ext)(papp_surface_t *s, const void *pixels, uint32_t flags);
} papp_if_disp_t;

/* ── AUDI interface ─────────────────────────────────────────────────────── */

#define PAPP_IF_AUDI      PAPP_FOURCC('A', 'U', 'D', 'I')
#define PAPP_IF_AUDI_VER  1

enum { PAPP_AUDIO_S16 = 1, PAPP_AUDIO_U8 = 2 };

typedef struct {
    uint32_t rate;               /* any rate 4000..192000; resampled by the launcher */
    uint8_t  channels;           /* 1 or 2 (interleaved)                     */
    uint8_t  format;             /* PAPP_AUDIO_*                             */
    uint16_t latency_ms;         /* ring size; 0 = default (100 ms)          */
    uint32_t reserved[2];
} papp_audio_cfg_t;

typedef struct {
    uint32_t queued_frames;      /* waiting in the ring (stream rate)        */
    uint32_t capacity_frames;
    uint32_t underruns;
    uint32_t reserved;
    uint64_t played_frames;      /* consumed by the mixer (stream rate)      */
} papp_audio_stats_t;

typedef struct papp_audio_stream papp_audio_stream_t;

typedef struct {
    papp_iface_hdr_t hdr;
    uint32_t             (*output_rate)(void);
    papp_audio_stream_t *(*open)(const papp_audio_cfg_t *cfg);           /* NULL on failure */
    void                 (*close)(papp_audio_stream_t *s);
    /* Queues frames; blocks up to timeout_ms for room. Returns frames queued. */
    size_t               (*write)(papp_audio_stream_t *s, const void *data, size_t frames,
                                  uint32_t timeout_ms);
    void                 (*pause)(papp_audio_stream_t *s, bool pause);
    void                 (*set_gain)(papp_audio_stream_t *s, int percent);  /* 0..100 */
    void                 (*clear)(papp_audio_stream_t *s);
    void                 (*stats)(papp_audio_stream_t *s, papp_audio_stats_t *out);
} papp_if_audi_t;

/* ── INPT interface ─────────────────────────────────────────────────────── */

#define PAPP_IF_INPT      PAPP_FOURCC('I', 'N', 'P', 'T')
#define PAPP_IF_INPT_VER  2      /* v2: analog */

#define PAPP_MAX_PLAYERS  4

/* Standard gamepad layout (positions, Xbox naming: A = bottom face button) */
#define PAPP_BTN_A        (1u << 0)
#define PAPP_BTN_B        (1u << 1)
#define PAPP_BTN_X        (1u << 2)
#define PAPP_BTN_Y        (1u << 3)
#define PAPP_BTN_L1       (1u << 4)
#define PAPP_BTN_R1       (1u << 5)
#define PAPP_BTN_L2       (1u << 6)
#define PAPP_BTN_R2       (1u << 7)
#define PAPP_BTN_SELECT   (1u << 8)
#define PAPP_BTN_START    (1u << 9)
#define PAPP_BTN_L3       (1u << 10)
#define PAPP_BTN_R3       (1u << 11)
#define PAPP_BTN_HOME     (1u << 12)   /* reserved for the launcher; apps see it but must not rely on it */
#define PAPP_BTN_MISC     (1u << 13)
#define PAPP_BTN_UP       (1u << 16)
#define PAPP_BTN_DOWN     (1u << 17)
#define PAPP_BTN_LEFT     (1u << 18)
#define PAPP_BTN_RIGHT    (1u << 19)

/* Analog inputs that are not part of a gamepad (INPT v2 analog()) */
#define PAPP_ANALOG_PADDLE 0      /* Atari paddle pot: 0..65535 */

typedef struct {
    uint8_t  connected;
    uint8_t  reserved;
    uint16_t vid, pid;
    uint16_t reserved2;
    uint32_t buttons;            /* PAPP_BTN_*                                */
    int16_t  lx, ly, rx, ry;     /* -32768..32767, +y = down                  */
    uint16_t lt, rt;             /* analog triggers 0..65535 (0 if digital)   */
    char     name[24];
} papp_gamepad_t;

/* Keyboard modifiers (HID boot report byte 0) */
#define PAPP_MOD_LCTRL    0x01
#define PAPP_MOD_LSHIFT   0x02
#define PAPP_MOD_LALT     0x04
#define PAPP_MOD_LGUI     0x08
#define PAPP_MOD_RCTRL    0x10
#define PAPP_MOD_RSHIFT   0x20
#define PAPP_MOD_RALT     0x40
#define PAPP_MOD_RGUI     0x80

enum {
    PAPP_EV_KEY_DOWN = 1,        /* code = HID usage, text = character or 0  */
    PAPP_EV_KEY_UP,
    PAPP_EV_BUTTON_DOWN,         /* device = player, code = bit index (0..31) */
    PAPP_EV_BUTTON_UP,
    PAPP_EV_DEVICE_ADDED,        /* device = player (0..3) or 0xFF keyboard   */
    PAPP_EV_DEVICE_REMOVED,
};

typedef struct {
    uint8_t  type;               /* PAPP_EV_*                                 */
    uint8_t  device;
    uint16_t code;
    uint16_t mods;               /* PAPP_MOD_* at the time of the event       */
    uint16_t text;               /* UTF-16 code unit (US layout for now)      */
    uint32_t time_ms;
} papp_input_event_t;

typedef struct {
    papp_iface_hdr_t hdr;
    bool     (*gamepad)(int player, papp_gamepad_t *out);   /* false if not connected */
    bool     (*key_down)(uint16_t usage);
    uint16_t (*modifiers)(void);
    bool     (*poll_event)(papp_input_event_t *out);        /* false if queue empty   */
    void     (*flush)(void);
    /* v2 */
    int      (*analog)(int ch);                           /* PAPP_ANALOG_*: 0..65535, -1 absent */
} papp_if_inpt_t;

/* ── THRD interface ─────────────────────────────────────────────────────── */
/* Threads and synchronisation. Everything is tracked per run: threads still
 * alive when the app ends are stopped and every object is freed. Waits never
 * block teardown. Timeouts are in ms; PAPP_WAIT_FOREVER waits indefinitely. */

#define PAPP_IF_THRD      PAPP_FOURCC('T', 'H', 'R', 'D')
#define PAPP_IF_THRD_VER  1

#define PAPP_WAIT_FOREVER 0xFFFFFFFFu
#define PAPP_MAX_THREADS  16                /* besides the main thread */

enum {
    PAPP_PRIO_LOW = 0,
    PAPP_PRIO_NORMAL,                       /* same as the main thread */
    PAPP_PRIO_HIGH,                         /* audio/emulation workers */
};

#define PAPP_THREAD_STACK_PSRAM  (1u << 0)  /* stack in PSRAM (slower, for big stacks) */

typedef struct {
    const char *name;            /* up to 15 chars; NULL = "papp_thr"         */
    uint32_t    stack_size;      /* bytes; 0 = 8 KB. > 32 KB goes to PSRAM     */
    uint8_t     prio;            /* PAPP_PRIO_*                               */
    int8_t      core;            /* -1 = either core, 0 or 1                  */
    uint16_t    flags;           /* PAPP_THREAD_*                             */
} papp_thread_cfg_t;

typedef int (*papp_thread_fn_t)(void *arg);

typedef struct papp_thread papp_thread_t;
typedef struct papp_mutex  papp_mutex_t;
typedef struct papp_sem    papp_sem_t;
typedef struct papp_queue  papp_queue_t;

typedef struct {
    papp_iface_hdr_t hdr;
    /* Threads. The thread ends when fn returns (its value is the exit code)
     * or calls thread_exit. Join every thread you create: it frees the slot. */
    papp_thread_t *(*thread_create)(const papp_thread_cfg_t *cfg, papp_thread_fn_t fn, void *arg);
    bool           (*thread_join)(papp_thread_t *t, uint32_t timeout_ms, int *exit_code);
    void           (*thread_exit)(int code);                 /* not from the main thread */
    papp_thread_t *(*thread_self)(void);                     /* NULL on the main thread  */
    void           (*yield)(void);
    /* One pointer-sized slot per thread (main included) for the app's own use;
     * the SDK keeps the C library's per-thread state here. */
    void         **(*tls_slot)(void);
    /* Ends the whole app with `code` from any thread (exit() from a worker). */
    void           (*app_exit)(int code);

    papp_mutex_t  *(*mutex_create)(bool recursive);
    void           (*mutex_destroy)(papp_mutex_t *m);
    bool           (*mutex_lock)(papp_mutex_t *m, uint32_t timeout_ms);
    void           (*mutex_unlock)(papp_mutex_t *m);

    papp_sem_t    *(*sem_create)(uint32_t initial, uint32_t max);
    void           (*sem_destroy)(papp_sem_t *s);
    bool           (*sem_take)(papp_sem_t *s, uint32_t timeout_ms);
    void           (*sem_give)(papp_sem_t *s);

    papp_queue_t  *(*queue_create)(uint32_t length, uint32_t item_size);
    void           (*queue_destroy)(papp_queue_t *q);
    bool           (*queue_send)(papp_queue_t *q, const void *item, uint32_t timeout_ms);
    bool           (*queue_recv)(papp_queue_t *q, void *item, uint32_t timeout_ms);
    uint32_t       (*queue_count)(papp_queue_t *q);
} papp_if_thrd_t;

/* ── UI interface ───────────────────────────────────────────────────────── */
/* Launcher-drawn screens an app can call up: the content browser (what every
 * emulator needs to pick a game) and a message box. They take over the
 * display and input until they return; the app's surface comes back on its
 * next present. They return early (CANCEL) when the app is asked to exit. */

#define PAPP_IF_UI        PAPP_FOURCC('U', 'I', ' ', ' ')
#define PAPP_IF_UI_VER    3      /* v2: menu, v3: volume */

typedef struct {
    const char *title;           /* header, e.g. "NES"                               */
    const char *root;            /* folder to browse, e.g. "/sd/roms/nes"             */
    const char *extensions;      /* ".nes,.fds" (case-insensitive); NULL = all files  */
    /* Save states: the save of ROM "<dir>/<file>" is "<save_dir>/<file><save_ext>"
     * (extension kept: "Zelda.nes" -> "Zelda.nes.sav", as RetroESP32 did).
     * With save_dir set, ROMs that have one offer Resume / Restart / Delete save. */
    const char *save_dir;
    const char *save_ext;        /* e.g. ".sav"                                       */
    uint32_t    flags;           /* PAPP_PICK_*_F                                     */
    const char *exclude;         /* with PAPP_PICK_EXCLUDE_F: "neogeo.zip,bios.col"   */
} papp_pick_cfg_t;

#define PAPP_PICK_SAVE_NOEXT_F (1u << 0)  /* save is "<save_dir>/<file without ext><save_ext>" */
#define PAPP_PICK_EXCLUDE_F    (1u << 1)  /* hide files named in cfg->exclude (case-insensitive) */
#define PAPP_PICK_NO_DIRS_F    (1u << 2)  /* list files only (no sub-folders) */
#define PAPP_PICK_FILES_F      (1u << 3)  /* file manager: real names, start at root, no recents */

enum {
    PAPP_PICK_CANCEL = 0,        /* user backed out (or the app must exit)            */
    PAPP_PICK_RUN,               /* start the ROM (it has no save)                    */
    PAPP_PICK_RESUME,            /* start the ROM and load save_path                  */
    PAPP_PICK_RESTART,           /* start fresh; the save stays until overwritten     */
};

typedef struct {
    int32_t action;              /* PAPP_PICK_*                                       */
    char    path[256];           /* the chosen ROM                                    */
    char    save_path[256];      /* where its save state lives ("" without save_dir)  */
} papp_pick_result_t;

typedef struct {
    papp_iface_hdr_t hdr;
    /* Browse cfg->root: folders, Favorites and Recent tabs, search, per-ROM
     * menu. Favorites and recents are kept by the launcher. Returns action. */
    int  (*pick_file)(const papp_pick_cfg_t *cfg, papp_pick_result_t *out);
    /* A message box; returns when the user dismisses it. */
    void (*message)(const char *title, const char *text);
    /* v2: a list of choices over the app's (dimmed) picture, e.g. an emulator's
     * in-game menu. Returns the index picked, or -1 for back / exit request.
     * At most 7 items. */
    int  (*menu)(const char *title, const char *const *items, int n, int initial);
    /* v3: the launcher's volume control over the app's picture (Y in the
     * emulators): adjusts and saves the master volume. Returns 0..100. */
    int  (*volume)(void);
} papp_if_ui_t;

/* ── Entry point ────────────────────────────────────────────────────────── */

/* Provided by the SDK runtime (crt0), located at entry_off. */
typedef int (*papp_start_fn_t)(int argc, char **argv);

#ifdef __cplusplus
}
#endif

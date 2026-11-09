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
#define PAPP_IF_UI_VER    4      /* v2: menu, v3: volume, v4: wifi */

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
    /* v4: the launcher's Wi-Fi picker: networks nearby, password on an
     * on-screen keyboard. The network joined is saved as the board's network
     * (/sd/config/wifi.txt) and stays after the app; the app never sees the
     * password. Returns 1 connected, 0 the user cancelled, or PAPP_E_EXIT /
     * PAPP_E_UNSUPPORTED (no Wi-Fi). Holds Wi-Fi for the run, like a NET call. */
    int  (*wifi)(void);
} papp_if_ui_t;

/* ── NET interface ──────────────────────────────────────────────────────── */
/* Wi-Fi, sockets, TLS and HTTP(S) through the launcher's network stack.
 * The radio is stopped while an app runs; the first NET call keeps it on for
 * the rest of the run. Everything the app opens is closed at teardown, a
 * SoftAP is stopped and a network joined with join() is left for the saved one.
 * Blocking calls take a timeout in ms (0 = don't wait, PAPP_WAIT_FOREVER) and
 * return PAPP_E_EXIT as soon as the app is asked to exit. One socket may be
 * used by a reader thread and a writer thread at once (not for TLS). */

#define PAPP_IF_NET       PAPP_FOURCC('N', 'E', 'T', ' ')
#define PAPP_IF_NET_VER   1

enum {                                   /* results: >= 0 success, these on failure */
    PAPP_E_OK = 0,
    PAPP_E_TIMEOUT = -1,
    PAPP_E_EXIT = -2,                    /* the app is asked to exit                 */
    PAPP_E_NOMEM = -3,
    PAPP_E_LIMIT = -4,                   /* too many sockets / sessions             */
    PAPP_E_NOT_CONNECTED = -5,           /* no network (or none saved)              */
    PAPP_E_REFUSED = -6,
    PAPP_E_RESET = -7,                   /* connection lost                          */
    PAPP_E_DNS = -8,
    PAPP_E_TLS = -9,
    PAPP_E_CERT = -10,                   /* server certificate not trusted          */
    PAPP_E_ARG = -11,
    PAPP_E_UNSUPPORTED = -12,            /* not on this board / C6 firmware          */
    PAPP_E_IO = -13,
    PAPP_E_HTTP = -14,                   /* malformed HTTP reply, too many redirects */
};

#define PAPP_NET_MAX_SOCKETS  8          /* TLS and HTTP use one each                */
#define PAPP_NET_MAX_TLS      2
#define PAPP_NET_MAX_HTTP     2

enum { PAPP_NET_OFF, PAPP_NET_IDLE, PAPP_NET_CONNECTING, PAPP_NET_CONNECTED, PAPP_NET_FAILED };

typedef struct {
    uint8_t  state;                      /* PAPP_NET_*                               */
    int8_t   rssi;                       /* dBm, 0 if not connected                  */
    uint8_t  channel;
    uint8_t  ap_clients;                 /* SoftAP: stations joined (0 if off)       */
    uint8_t  ap_running;
    uint8_t  reserved[3];
    char     ssid[33];                   /* network joined (or being joined)         */
    uint8_t  mac[6];
    uint8_t  ip[4], mask[4], gateway[4], dns[4];
    uint8_t  ap_ip[4];                   /* SoftAP address (192.168.4.1)             */
} papp_net_info_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t secure;                      /* needs a password                         */
} papp_net_ap_t;

enum { PAPP_AF_INET = 4 };
typedef struct {
    uint8_t  family;                     /* PAPP_AF_INET                             */
    uint8_t  reserved;
    uint16_t port;                       /* host byte order                          */
    uint8_t  ip[16];                     /* IPv4 in ip[0..3]; room for IPv6          */
} papp_addr_t;

typedef struct papp_sock papp_sock_t;
enum { PAPP_SOCK_TCP = 1, PAPP_SOCK_UDP = 2 };

enum {                                   /* set_opt */
    PAPP_SO_NODELAY = 1,                 /* TCP: 0/1                                 */
    PAPP_SO_KEEPALIVE,                   /* TCP: idle seconds, 0 = off               */
    PAPP_SO_BROADCAST,                   /* UDP: 0/1                                 */
    PAPP_SO_MCAST_JOIN,                  /* UDP: IPv4 group, a.b.c.d = a | b<<8 | c<<16 | d<<24 */
    PAPP_SO_MCAST_LEAVE,
    PAPP_SO_REUSEADDR,                   /* 0/1, before bind                         */
};

enum { PAPP_POLL_IN = 1, PAPP_POLL_OUT = 2, PAPP_POLL_ERR = 4 };
typedef struct {
    papp_sock_t *sock;
    uint8_t      want;                   /* PAPP_POLL_IN / OUT                       */
    uint8_t      got;                    /* filled in                                */
} papp_poll_t;

typedef struct {
    const char  *ca_pem;                 /* NULL = the launcher's certificate bundle */
    size_t       ca_len;                 /* with the terminating 0                   */
    const char **alpn;                   /* NULL-terminated list, or NULL            */
    uint32_t     timeout_ms;             /* resolve + connect + handshake; 0 = 15 s  */
    uint8_t      insecure;               /* 1 = don't check the certificate (tests)  */
} papp_tls_cfg_t;

typedef struct papp_http papp_http_t;
typedef struct {
    const char *method;                  /* "GET", "POST", ...; NULL = GET           */
    const char *url;                     /* http:// or https://                      */
    const char *headers;                 /* extra "Name: value\r\n" lines, or NULL   */
    const void *body;                    /* sent with Content-Length; NULL = none    */
    size_t      body_len;
    uint32_t    timeout_ms;              /* per wait; 0 = 15 s                       */
    uint8_t     max_redirects;           /* 0 = 5                                    */
    uint8_t     insecure;                /* https without certificate check (tests)  */
} papp_http_req_t;

enum {
    PAPP_NET_EV_CONNECTED = 1,           /* got an address                           */
    PAPP_NET_EV_DISCONNECTED,            /* code = Wi-Fi reason                      */
    PAPP_NET_EV_SCAN_DONE,               /* code = networks found                    */
    PAPP_NET_EV_AP_CLIENT_JOINED,        /* code = stations now on the SoftAP        */
    PAPP_NET_EV_AP_CLIENT_LEFT,
};
typedef struct {
    uint8_t  type;                       /* PAPP_NET_EV_*                            */
    uint8_t  reserved;
    uint16_t code;
    uint32_t time_ms;
} papp_net_event_t;

typedef struct {
    papp_iface_hdr_t hdr;

    /* Wi-Fi station. up() joins the launcher's saved network (Settings > Wi-Fi)
     * and waits for an address: PAPP_E_OK, or NOT_CONNECTED (none saved / failed). */
    int   (*up)(uint32_t timeout_ms);
    void  (*info)(papp_net_info_t *out);
    int   (*scan)(papp_net_ap_t *out, int max, uint32_t timeout_ms);   /* count, strongest first */
    /* Another network for this run only: nothing is saved, the saved one is back after the app. */
    int   (*join)(const char *ssid, const char *password, uint32_t timeout_ms);
    bool  (*poll_event)(papp_net_event_t *out);

    /* SoftAP beside the station (local multiplayer, setup pages); open if password is NULL/"".
     * channel 0 = the station's. UNSUPPORTED where the C6 firmware can't. */
    int   (*ap_start)(const char *ssid, const char *password, uint8_t channel, uint8_t max_clients);
    void  (*ap_stop)(void);

    /* Name or "a.b.c.d" to an address (port left 0). */
    int   (*resolve)(const char *host, papp_addr_t *out, uint32_t timeout_ms);

    /* Sockets. NULL from socket()/accept() on failure (err gets the reason when not NULL). */
    papp_sock_t *(*socket)(int type, int *err);
    int   (*connect)(papp_sock_t *s, const papp_addr_t *to, uint32_t timeout_ms);
    int   (*bind)(papp_sock_t *s, uint16_t port);                    /* all local addresses   */
    int   (*listen)(papp_sock_t *s, int backlog);
    papp_sock_t *(*accept)(papp_sock_t *s, papp_addr_t *from, uint32_t timeout_ms, int *err);
    int   (*send)(papp_sock_t *s, const void *buf, size_t len, uint32_t timeout_ms);   /* bytes sent */
    int   (*recv)(papp_sock_t *s, void *buf, size_t len, uint32_t timeout_ms);  /* 0 = peer closed */
    int   (*sendto)(papp_sock_t *s, const void *buf, size_t len, const papp_addr_t *to);
    int   (*recvfrom)(papp_sock_t *s, void *buf, size_t len, papp_addr_t *from, uint32_t timeout_ms);
    int   (*set_opt)(papp_sock_t *s, int opt, int value);
    int   (*poll)(papp_poll_t *set, int n, uint32_t timeout_ms);    /* sockets ready, 0 = timeout */
    int   (*local_addr)(papp_sock_t *s, papp_addr_t *out);          /* after bind / connect  */
    void  (*close)(papp_sock_t *s);

    /* TLS client over TCP: the returned socket works with send/recv/poll/close. */
    papp_sock_t *(*tls_connect)(const char *host, uint16_t port, const papp_tls_cfg_t *cfg, int *err);

    /* HTTP/1.1 client. http_open sends the request, follows redirects and reads the
     * reply headers: the result is the status (200...) or a PAPP_E_* code. */
    papp_http_t *(*http_open)(const papp_http_req_t *req, int *status);
    int   (*http_header)(papp_http_t *h, const char *name, char *out, size_t out_len);  /* length, -1 */
    long  (*http_length)(papp_http_t *h);                    /* Content-Length, -1 unknown/chunked */
    int   (*http_read)(papp_http_t *h, void *buf, size_t len);   /* bytes, 0 = end of body       */
    void  (*http_close)(papp_http_t *h);
} papp_if_net_t;

/* ── BLE interface ──────────────────────────────────────────────────────── */
/* Bluetooth LE through the launcher's NimBLE host (the controller is on the
 * C6). The host starts on the first call and is shut down at teardown, with
 * advertising, scans, connections and the GATT table. Results use the
 * PAPP_E_* codes of NET (>= 0 success). Blocking calls take a timeout in ms
 * and return PAPP_E_EXIT as soon as the app is asked to exit. No pairing in
 * v1: characteristics are open. */

#define PAPP_IF_BLE       PAPP_FOURCC('B', 'L', 'E', ' ')
#define PAPP_IF_BLE_VER   1

#define PAPP_BLE_MAX_CONN     3
#define PAPP_BLE_MAX_SVCS     8
#define PAPP_BLE_MAX_CHRS     32
#define PAPP_BLE_CONN_NONE    0xFFFF
#define PAPP_BLE_CONN_ALL     0xFFFF     /* notify: every subscribed peer               */

typedef struct { uint8_t type; uint8_t a[6]; } papp_ble_addr_t;   /* type 0 public, 1 random; a[0] = last byte */
typedef struct { uint8_t len; uint8_t u[16]; } papp_ble_uuid_t;   /* len 2, 4 or 16; little endian (as on air) */
typedef uint16_t papp_ble_conn_t;

/* Scanning (central) */
typedef struct {
    uint8_t  active;                     /* ask for scan responses                      */
    uint8_t  filter_duplicates;
    uint16_t interval_ms, window_ms;     /* 0 = defaults                                */
    uint32_t duration_ms;                /* 0 = until scan_stop                         */
} papp_ble_scan_cfg_t;

/* Advertising (peripheral) */
typedef struct {
    const uint8_t *adv;  uint8_t adv_len;      /* raw AD structures, <= 31 bytes         */
    const uint8_t *rsp;  uint8_t rsp_len;      /* scan response, <= 31 bytes (optional)  */
    uint8_t  connectable;
    uint16_t interval_ms;                      /* 0 = 100 ms                             */
} papp_ble_adv_cfg_t;

/* GATT server: the app describes its services once; reads return the stored value */
enum {
    PAPP_BLE_READ = 1, PAPP_BLE_WRITE = 2, PAPP_BLE_WRITE_NR = 4,     /* write without response */
    PAPP_BLE_NOTIFY = 8, PAPP_BLE_INDICATE = 16,
};
typedef struct {
    papp_ble_uuid_t uuid;
    uint16_t        flags;               /* PAPP_BLE_*                                  */
    uint16_t        max_len;             /* value size, 1..512                          */
    uint16_t       *handle;              /* filled in by gatt_register (value handle)   */
} papp_ble_chr_t;
typedef struct {
    papp_ble_uuid_t       uuid;
    const papp_ble_chr_t *chrs;
    uint8_t               chr_count;
} papp_ble_svc_t;

/* GATT client discovery */
typedef struct {
    papp_ble_uuid_t uuid;
    uint16_t        handle;              /* value handle                                */
    uint16_t        cccd;                /* 0: none (no notify/indicate)                */
    uint16_t        flags;               /* PAPP_BLE_*                                  */
    uint16_t        svc_index;           /* which entry of the services array           */
} papp_ble_found_chr_t;
typedef struct { papp_ble_uuid_t uuid; uint16_t start, end; } papp_ble_found_svc_t;

/* Events. Data is copied into the event, up to 244 bytes (more: read the value). */
enum {
    PAPP_BLE_EV_ADV = 1,                 /* addr, rssi, code = adv type, data = AD structures */
    PAPP_BLE_EV_SCAN_DONE,
    PAPP_BLE_EV_CONNECTED,               /* conn, addr; code = 0 or an error            */
    PAPP_BLE_EV_DISCONNECTED,            /* conn, code = HCI reason                     */
    PAPP_BLE_EV_NOTIFY,                  /* client: conn, handle, data; code 1 = indication */
    PAPP_BLE_EV_WRITE,                   /* server: a peer wrote conn, handle, data     */
    PAPP_BLE_EV_SUBSCRIBE,               /* server: conn, handle; code 0 off, 1 notify, 2 indicate */
    PAPP_BLE_EV_MTU,                     /* conn, code = new MTU                        */
    PAPP_BLE_EV_ADV_DONE,                /* advertising stopped (a central connected)  */
};
typedef struct {
    uint8_t         type;
    int8_t          rssi;
    papp_ble_conn_t conn;
    uint16_t        handle;
    uint16_t        code;
    papp_ble_addr_t addr;
    uint16_t        len;
    uint8_t         data[244];
} papp_ble_event_t;

typedef struct {
    papp_iface_hdr_t hdr;
    bool  (*available)(void);            /* starts the host; false: no BLE on this board/C6 */
    int   (*own_addr)(papp_ble_addr_t *out);
    bool  (*poll_event)(papp_ble_event_t *out);
    bool  (*wait_event)(papp_ble_event_t *out, uint32_t timeout_ms);  /* false: timeout or exit */

    /* Central */
    int   (*scan_start)(const papp_ble_scan_cfg_t *cfg);
    void  (*scan_stop)(void);
    int   (*connect)(const papp_ble_addr_t *peer, uint32_t timeout_ms, papp_ble_conn_t *out);
    void  (*disconnect)(papp_ble_conn_t c);
    int   (*mtu_exchange)(papp_ble_conn_t c, uint32_t timeout_ms);       /* result: the MTU */
    int   (*discover)(papp_ble_conn_t c, papp_ble_found_svc_t *svcs, int max_svcs, int *n_svcs,
                      papp_ble_found_chr_t *chrs, int max_chrs, int *n_chrs, uint32_t timeout_ms);
    int   (*read)(papp_ble_conn_t c, uint16_t handle, void *buf, size_t len, uint32_t timeout_ms); /* bytes */
    int   (*write)(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len, bool response,
                   uint32_t timeout_ms);
    int   (*subscribe)(papp_ble_conn_t c, uint16_t cccd, int mode, uint32_t timeout_ms);  /* 0 off, 1, 2 */

    /* Peripheral */
    int   (*gatt_register)(const papp_ble_svc_t *svcs, int count);   /* once, before adv_start */
    int   (*set_value)(uint16_t handle, const void *buf, size_t len);     /* what reads return */
    int   (*notify)(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len); /* also sets the value */
    int   (*adv_start)(const papp_ble_adv_cfg_t *cfg);
    void  (*adv_stop)(void);
    int   (*set_name)(const char *name);                             /* GAP device name   */
} papp_if_ble_t;

/* ── Entry point ────────────────────────────────────────────────────────── */

/* Provided by the SDK runtime (crt0), located at entry_off. */
typedef int (*papp_start_fn_t)(int argc, char **argv);

#ifdef __cplusplus
}
#endif

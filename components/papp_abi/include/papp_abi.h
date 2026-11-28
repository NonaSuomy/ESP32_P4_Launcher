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
#define PAPP_IF_SYS_VER   5      /* v2: local_time; v3: utc_time; v4: chip_rev; v5: random */

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
    /* v4: the chip revision, major * 100 + minor (v1.3 = 103, v3.2 = 302). Code using the P4's SIMD
     * extension depends on it: v1.x chips have PIE (-march ..._xesppie), v3.x the re-encoded _xespv;
     * the same instructions with other encodings, so each faults on the other generation. */
    int      (*chip_rev)(void);
    /* v5: bytes from the chip's hardware random number generator (esp_fill_random): true random while
     * a noise source feeds it, which ESP-IDF keeps on for chips without a radio of their own, such as the
     * ESP32-P4 (its SAR ADC's noise). Good for keys and nonces. */
    void     (*random)(void *buf, size_t len);
} papp_if_sys_t;

/* ── MEM interface ──────────────────────────────────────────────────────── */

#define PAPP_IF_MEM       PAPP_FOURCC('M', 'E', 'M', ' ')
#define PAPP_IF_MEM_VER   3           /* v2: code_alloc / code_free / code_sync; v3: PAPP_MEM_LPRAM */

/* Abstract capabilities — mapped by the launcher, never ESP-IDF values. */
#define PAPP_MEM_ANY      0u
#define PAPP_MEM_FAST     (1u << 0)   /* internal SRAM                       */
#define PAPP_MEM_DMA      (1u << 1)   /* usable by DMA / PPA (64 B aligned)  */
#define PAPP_MEM_LARGE    (1u << 2)   /* PSRAM                               */
#define PAPP_MEM_LPRAM    (1u << 3)   /* v3: the P4's low-power RAM, ~30 KB, about 50x slower than
                                       * SRAM (uncached): only for data that is rarely touched. Never
                                       * given out otherwise. Other caps are ignored; not DMA-capable. */

typedef struct {
    papp_iface_hdr_t hdr;
    void *(*alloc)(size_t size, uint32_t caps, size_t align);
    void *(*realloc)(void *ptr, size_t size);
    void  (*free)(void *ptr);
    size_t (*free_size)(uint32_t caps);
    /* v2: memory for generated code (a JIT). PSRAM the app can write and run at the same
     * address; whole 64 KB pages. After writing code, call code_sync on the range before
     * running it (writes back the data cache, drops stale instruction lines). */
    void *(*code_alloc)(size_t size);
    void  (*code_free)(void *code);
    int   (*code_sync)(void *code, size_t size);     /* PAPP_E_OK, PAPP_E_ARG */
} papp_if_mem_t;

/* ── FS interface ───────────────────────────────────────────────────────── */

#define PAPP_IF_FS        PAPP_FOURCC('F', 'S', ' ', ' ')
#define PAPP_IF_FS_VER    3      /* v2: stat, rename, rmdir, directories; v3: volume_info */

typedef struct papp_file papp_file_t;
typedef struct papp_dir  papp_dir_t;

typedef struct {
    int64_t  size;                       /* bytes (0 for a directory)                */
    int64_t  mtime;                      /* seconds since 1970, 0 if unknown        */
    uint8_t  is_dir;
    uint8_t  reserved[7];
} papp_stat_t;

typedef struct {
    char     name[256];                  /* the entry's name, not its path          */
    uint8_t  is_dir;
    uint8_t  reserved[3];
} papp_dirent_t;

#define PAPP_FS_MAX_DIRS 8               /* open at once; closed with the app       */

enum {
    PAPP_FS_TYPE_UNKNOWN = 0,
    PAPP_FS_TYPE_FAT12 = 1,
    PAPP_FS_TYPE_FAT16 = 2,
    PAPP_FS_TYPE_FAT32 = 3,
    PAPP_FS_TYPE_EXFAT = 4,
};

typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t type;                       /* PAPP_FS_TYPE_* */
    uint32_t reserved;
} papp_fs_volume_info_t;

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
    /* v2. 0 on success, -1 on failure. */
    int     (*stat)(const char *path, papp_stat_t *out);
    int     (*rename)(const char *from, const char *to);
    int     (*rmdir)(const char *path);                  /* empty directories only  */
    papp_dir_t *(*opendir)(const char *path);            /* NULL: missing / no slot */
    int     (*readdir)(papp_dir_t *d, papp_dirent_t *out);   /* 1 an entry, 0 the end ("." and ".." left out) */
    void    (*closedir)(papp_dir_t *d);
    int     (*volume_info)(const char *mount_path, papp_fs_volume_info_t *out); /* v3: mounted FAT volume */
} papp_if_fs_t;

/* ── DISP interface ─────────────────────────────────────────────────────── */

#define PAPP_IF_DISP      PAPP_FOURCC('D', 'I', 'S', 'P')
#define PAPP_IF_DISP_VER  5      /* v2: touch_read(NULL) = display coordinates; v3: present_ext; v4: blit;
                                    v5: blit_yuv */

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

/* DISP v4 blit: a picture drawn by the PPA, the P4's picture engine (sprites) */
enum {
    PAPP_BLIT_RGB565   = 0,              /* uint16_t pixels                                   */
    PAPP_BLIT_ARGB8888 = 1,              /* uint32_t 0xAARRGGBB: alpha per pixel               */
    PAPP_BLIT_A8       = 2,              /* uint8_t alpha only, drawn in .color (text, icons)  */
};
#define PAPP_BLIT_COLORKEY  (1u << 0)    /* pixels of colour .key are not drawn                */
#define PAPP_BLIT_ALPHA     (1u << 1)    /* the whole picture at .alpha (0..255), on top of its own */
#define PAPP_BLIT_MIRROR_X  (1u << 2)    /* the drawn picture flipped left-right (after .rotate) */
#define PAPP_BLIT_MIRROR_Y  (1u << 3)    /* the drawn picture flipped upside down (after .rotate) */
typedef struct {
    const void *pixels;                  /* .format, w x h, stride pixels per line (0 = w)     */
    uint16_t    w, h, stride;
    uint8_t     format;                  /* PAPP_BLIT_RGB565 / _ARGB8888 / _A8                 */
    uint8_t     flags;                   /* PAPP_BLIT_*                                        */
    int16_t     x, y;                    /* top left of the drawn picture (after scaling and
                                            turning) on the surface; may be partly outside     */
    uint16_t    key;                     /* RGB565 colour left out (PAPP_BLIT_COLORKEY)        */
    uint16_t    color;                   /* PAPP_BLIT_A8: the colour (RGB565)                  */
    uint8_t     alpha;                   /* PAPP_BLIT_ALPHA                                    */
    uint8_t     rotate;                  /* quarter turns counter-clockwise, 0..3              */
    uint16_t    scale_x, scale_y;        /* 256 = 1.0 (0 = 1.0 too), in steps of 1/16, from 1/16
                                            to 16; along the picture's own x and y, before turning.
                                            Scaling is smooth (bilinear): with a colour key the edge
                                            pixels mix with the key colour, so scale ARGB8888 sprites */
    uint32_t    reserved[2];             /* 0 */
} papp_blit_t;

/* DISP v5 blit_yuv: a decoded video frame (planar YUV 4:2:0, as FFmpeg's yuv420p / I420), turned into
 * RGB565 and scaled by the PPA straight into the surface: no colour conversion or scaling on the CPU. */
#define PAPP_YUV_BT709       (1u << 0)   /* BT.709 colours (HD video); else BT.601 (SD video, JPEG)  */
#define PAPP_YUV_FULL_RANGE  (1u << 1)   /* 0..255 (JPEG); else 16..235 (video)                     */
typedef struct {
    const uint8_t *y, *u, *v;            /* the planes: u and v at half the width and height        */
    uint16_t    w, h;                    /* even, at least 2 x 2                                     */
    uint16_t    stride_y, stride_uv;     /* bytes per line; 0 = w, w / 2                             */
    int16_t     dx, dy;                  /* top left of the drawn (scaled) frame on the surface: it must
                                            lie wholly inside the surface (PAPP_E_ARG otherwise)    */
    uint16_t    scale_x, scale_y;        /* as papp_blit_t: 256 = 1.0 (0 = 1.0), steps of 1/16      */
    uint8_t     flags;                   /* PAPP_YUV_*                                               */
    uint8_t     reserved8[3];
    uint32_t    reserved[2];             /* 0 */
} papp_yuv_t;

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
    /* v4: the PPA draws a picture into the buffer lock() returned, clipped to the surface:
     * scaled, turned and mirrored (RGB565 / ARGB8888), then mixed in with its alpha, the colour
     * key and .alpha. The PPA works in memory, so finish the CPU's drawing under it first.
     * 0 = ok, PAPP_E_ARG. Scaling, turning and mirroring are not for PAPP_BLIT_A8. */
    int             (*blit)(papp_surface_t *s, const papp_blit_t *b);
    /* v4: the PPA fills a rectangle of that buffer with an RGB565 colour (clipped). 0 = ok */
    int             (*fill)(papp_surface_t *s, int x, int y, int w, int h, uint16_t color);
    /* v5: a YUV 4:2:0 frame (papp_yuv_t) drawn by the PPA into the buffer lock() returned, replacing
     * what was there. As with blit, finish the CPU's drawing first; drawing on top of it after is fine.
     * 0 = ok, PAPP_E_ARG, PAPP_E_NOMEM. */
    int             (*blit_yuv)(papp_surface_t *s, const papp_yuv_t *f);
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
#define PAPP_IF_INPT_VER  3      /* v2: analog, v3: keyboard */

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
#define PAPP_ANALOG_ACCEL_X 16    /* accelerometer, on boards that have one: 32768 = 0 g, 4096 per g, */
#define PAPP_ANALOG_ACCEL_Y 17    /* the board's own axes. -1 on a board without, or a launcher before  */
#define PAPP_ANALOG_ACCEL_Z 18    /* these were added. The first read may take a moment (it wakes the chip) */

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
    /* v3 */
    bool     (*keyboard)(void);                           /* a keyboard is attached (USB or the board's own) */
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
 * and return PAPP_E_EXIT as soon as the app is asked to exit. v1 has no
 * pairing: characteristics are open. v2 adds pairing and
 * bonds kept across runs, characteristics that need an encrypted link,
 * descriptors, the GAP appearance and a static random address: enough for a
 * HID over GATT keyboard. */

#define PAPP_IF_BLE       PAPP_FOURCC('B', 'L', 'E', ' ')
#define PAPP_IF_BLE_VER   2      /* v2: security, bonds, gatt_register2, appearance, random address */

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
    PAPP_BLE_READ_ENC = 32, PAPP_BLE_WRITE_ENC = 64,    /* v2: only on an encrypted link (a host pairs first) */
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

/* v2: a characteristic with descriptors of fixed value (a HID Report Reference, 0x2908, say). */
typedef struct {
    papp_ble_uuid_t uuid;
    uint8_t         value[8];
    uint8_t         len;
    uint8_t         flags;               /* PAPP_BLE_READ, optionally PAPP_BLE_READ_ENC */
} papp_ble_dsc_t;
typedef struct {
    papp_ble_uuid_t       uuid;
    uint16_t              flags;         /* PAPP_BLE_*                                  */
    uint16_t              max_len;       /* value size, 1..512                          */
    uint16_t             *handle;        /* filled in by gatt_register2 (value handle)  */
    const papp_ble_dsc_t *dscs;
    uint8_t               dsc_count;     /* up to 4                                     */
} papp_ble_chr2_t;
typedef struct {
    papp_ble_uuid_t        uuid;
    const papp_ble_chr2_t *chrs;
    uint8_t                chr_count;
} papp_ble_svc2_t;

/* v2: pairing. Set before advertising; bonds are kept on the SD card (/sd/config/ble_bonds.bin) for later runs. */
enum { PAPP_BLE_IO_DISPLAY_ONLY = 0, PAPP_BLE_IO_DISPLAY_YESNO, PAPP_BLE_IO_KEYBOARD_ONLY,
       PAPP_BLE_IO_NONE, PAPP_BLE_IO_KEYBOARD_DISPLAY };
typedef struct {
    uint8_t io_cap;                      /* PAPP_BLE_IO_* (NONE: "Just Works")          */
    uint8_t bond;                        /* keep the keys for the next connection      */
    uint8_t mitm;
    uint8_t sc;                          /* LE Secure Connections (else legacy pairing) */
    uint8_t initiate;                    /* ask a central for encryption as soon as it
                                          * connects (macOS waits to be asked)          */
} papp_ble_sec_cfg_t;

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
    PAPP_BLE_EV_ENCRYPTED,               /* v2: conn; code 0 = encrypted, else the pairing failed (NimBLE status) */
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

    /* v2 */
    int   (*gatt_register2)(const papp_ble_svc2_t *svcs, int count);  /* as gatt_register, with descriptors */
    int   (*security)(const papp_ble_sec_cfg_t *cfg);
    int   (*set_appearance)(uint16_t appearance);                     /* GAP appearance (0x03C1 keyboard) */
    /* Advertise and connect from this static random address (6 bytes, least significant first; the two top
     * bits of a[5] set), or NULL for the board's public one. Before advertising. */
    int   (*set_random_addr)(const uint8_t a[6]);
    int   (*forget_bonds)(void);                                      /* every host forgotten */
    int   (*bond_count)(void);
} papp_if_ble_t;

/* ── USB host interface ─────────────────────────────────────────────────── */
/* Raw access to devices on the board's USB host port, for devices the launcher's own input
 * drivers do not take (programmers, microcontrollers in their bootloaders, serial adapters):
 * open one, claim an interface, then control, bulk and interrupt transfers. The launcher's HID
 * and XInput drivers keep their interfaces; an app claims only interfaces nobody else has.
 * Everything blocks; transfers return the bytes moved or a PAPP_E_* code of NET (PAPP_E_IO: the
 * device stalled or the transfer failed; PAPP_E_RESET: the device was unplugged; PAPP_E_EXIT
 * as soon as the app is asked to exit). One transfer at a time per endpoint (a second caller on it
 * waits); different endpoints run side by side, so a thread can wait on an IN endpoint while others
 * write and send control requests. close() ends transfers still waiting (PAPP_E_RESET). Devices
 * the app leaves open are closed when it exits. The port runs at full speed (12 Mbit/s). */

#define PAPP_IF_USBH      PAPP_FOURCC('U', 'S', 'B', 'H')
#define PAPP_IF_USBH_VER  1

#define PAPP_USBH_MAX_DEVS 4             /* open at once                                */

typedef struct papp_usb_dev papp_usb_dev_t;

typedef struct {
    uint8_t  addr;                       /* bus address: what open() takes              */
    uint8_t  speed;                      /* 0 low, 1 full, 2 high                       */
    uint16_t vid, pid, bcd_device;
    uint8_t  dev_class, dev_subclass, dev_protocol;
    uint8_t  num_interfaces;             /* in the active configuration                 */
    char     manufacturer[32], product[48], serial[48];    /* ASCII, "" if none   */
} papp_usb_info_t;

typedef struct {
    papp_iface_hdr_t hdr;
    /* The devices on the bus now (up to max): the count, or PAPP_E_UNSUPPORTED (no USB host). */
    int   (*list)(papp_usb_info_t *out, int max);
    /* Bumps whenever a device arrives or leaves: poll it to notice either without listing. */
    uint32_t (*generation)(void);
    papp_usb_dev_t *(*open)(uint8_t addr, int *err);
    void  (*close)(papp_usb_dev_t *d);                     /* releases what it claimed       */
    int   (*info)(papp_usb_dev_t *d, papp_usb_info_t *out);
    /* The active configuration descriptor, whole (copied up to len): its total length. */
    int   (*config_descriptor)(papp_usb_dev_t *d, void *buf, size_t len);
    int   (*claim)(papp_usb_dev_t *d, uint8_t interface, uint8_t alt);
    int   (*release)(papp_usb_dev_t *d, uint8_t interface);
    /* A control transfer on endpoint 0: data is read when bit 7 of type is set, else written. */
    int   (*control)(papp_usb_dev_t *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                     void *data, uint16_t len, uint32_t timeout_ms);
    /* A bulk or interrupt transfer on a claimed interface's endpoint (bit 7: IN). An IN reads up
     * to len bytes (a short packet ends it); timeout 0 waits for ever. */
    int   (*transfer)(papp_usb_dev_t *d, uint8_t ep, void *data, size_t len, uint32_t timeout_ms);
    /* After a stall or a timeout: the host's side of the endpoint flushed and its halt cleared.
     * The device's halt is the app's to clear (CLEAR_FEATURE through control). */
    int   (*clear_halt)(papp_usb_dev_t *d, uint8_t ep);
    bool  (*gone)(papp_usb_dev_t *d);                      /* unplugged since it was opened  */
} papp_if_usbh_t;

/* ── PWR interface ──────────────────────────────────────────────────────── */
/* The battery and its charging, on boards that measure it. Charging is on whenever the launcher runs; an app
 * may stop it for a while (a USB device it powers that wants the full 5 V while the battery charges, as a T48
 * programmer does), and it comes back on when the app ends, however it ends. */

#define PAPP_IF_PWR       PAPP_FOURCC('P', 'W', 'R', ' ')
#define PAPP_IF_PWR_VER   1

typedef struct {
    int32_t  pack_mv;                    /* the whole battery; 0 if not measured           */
    int32_t  cell_mv;                    /* a cell's share                                 */
    int32_t  ma;                         /* positive = charging, negative = running on it  */
    int8_t   percent;                    /* 0..100 by voltage, -1 unknown                  */
    uint8_t  charging;                   /* the charger says it is charging                */
    uint8_t  charge_enabled;             /* charging allowed now                           */
    uint8_t  can_switch;                 /* set_charge works on this board                 */
} papp_power_t;

typedef struct {
    papp_iface_hdr_t hdr;
    int   (*read)(papp_power_t *out);    /* PAPP_E_OK, or PAPP_E_UNSUPPORTED (no battery reading) */
    int   (*set_charge)(bool on);        /* PAPP_E_OK, or PAPP_E_UNSUPPORTED                    */
} papp_if_pwr_t;

/* ── HIDD interface ─────────────────────────────────────────────────────── */
/* The board's USB device port (the Tab5's USB-C) as a USB HID device of the app's making: a keyboard, a
 * mouse, media keys, under one HID interface with the app's report descriptor. start() takes the port from
 * the launcher's own USB link (DevLink, the USB serial/JTAG unit), which is gone from the computer until
 * stop() or the app's end, when the launcher takes the port back. Reports are queued and go out one per USB
 * frame in order, so a press and its release are never merged. */

#define PAPP_IF_HIDD      PAPP_FOURCC('H', 'I', 'D', 'D')
#define PAPP_IF_HIDD_VER  1

typedef struct {
    const char    *manufacturer;         /* NULL: "P4 Launcher"                            */
    const char    *product;              /* NULL: the app's name                           */
    uint16_t       vid, pid;             /* 0: Espressif's 0x303A and a development PID   */
    const uint8_t *report_desc;          /* copied; up to 512 bytes                        */
    uint16_t       report_desc_len;
    uint8_t        boot_keyboard_id;     /* the report ID that is a boot keyboard (BIOS,
                                          * KVMs): the interface says so, and in boot
                                          * protocol that report goes without its ID; 0 none */
} papp_hidd_cfg_t;

enum {                                   /* state()                                      */
    PAPP_HIDD_RUNNING = 1,               /* the port is the app's                          */
    PAPP_HIDD_MOUNTED = 2,               /* a host has set it up                           */
    PAPP_HIDD_SUSPENDED = 4,             /* the host sleeps (or the cable has no data)     */
    PAPP_HIDD_BOOT = 8,                  /* the host asked for boot protocol               */
};

typedef struct {
    papp_iface_hdr_t hdr;
    int     (*start)(const papp_hidd_cfg_t *cfg);   /* PAPP_E_OK, ARG, UNSUPPORTED, NOMEM   */
    void    (*stop)(void);
    /* Queues a report (len <= 16). wait_ms: it goes no sooner than that long after the one
     * before it (macOS ignores a Caps Lock tap shorter than about 0.1 s). PAPP_E_LIMIT: the
     * queue is full (the host is not reading); nothing is queued while no host has set it up. */
    int     (*report)(uint8_t id, const void *data, uint8_t len, uint16_t wait_ms);
    uint8_t (*leds)(void);                           /* the host's keyboard LEDs (Caps = 2)  */
    int     (*state)(void);                          /* PAPP_HIDD_* bits                     */
} papp_if_hidd_t;

/* ── IRQ interface ──────────────────────────────────────────────────────── */
/* GPIO pins and hardware timers, with their interrupts on the core the app picks. Only the pins the board
 * leaves free (gpio_mask(): header and Grove pins; never the display, SD card, Wi-Fi chip or audio) can be
 * used. Everything is undone when the app ends, however it ends: interrupts freed, timers deleted, pins back
 * to inputs without pulls.
 *
 * An interrupt is handled in one of two ways (papp_irq_cfg_t.isr):
 *   deferred (isr NULL): the launcher's interrupt handler, on cfg.core, queues an event; a thread of the app
 *     takes it with wait() (papp_irq_thread() makes one that calls a function per event). The thread can do
 *     anything; it starts some 10-50 us after the edge.
 *   direct (isr set): the app's function runs inside the interrupt itself, on cfg.core. It must be tagged
 *     PAPP_FAST and the app's FAST region must be in internal SRAM (attach answers PAPP_E_UNSUPPORTED
 *     otherwise), because it also runs while the caches are off (flash writes, code_alloc). Inside it:
 *     no service calls and no papp_* SDK calls (they are in PSRAM), no floating point, under 1 KB of stack,
 *     only data tagged PAPP_FAST_DATA / PAPP_FAST_BSS or on its stack, and only what ctx offers: read and
 *     write pins, queue an event for wait() on any of its handles (signal), re-arm a timer. Keep it to a few microseconds.
 * Level interrupts (PAPP_IRQ_LOW / HIGH) fire once and stay off until enable(h, true): the level would
 * otherwise interrupt again at once. */

#define PAPP_IF_IRQ       PAPP_FOURCC('I', 'R', 'Q', ' ')
#define PAPP_IF_IRQ_VER   1

#define PAPP_IRQ_MAX_PINS    8               /* pins with an interrupt, per app              */
#define PAPP_IRQ_MAX_TIMERS  2               /* hardware timers, per app                     */

enum { PAPP_GPIO_INPUT = 0, PAPP_GPIO_OUTPUT = 1, PAPP_GPIO_OPEN_DRAIN = 2 };   /* gpio_config mode */
enum { PAPP_GPIO_PULL_NONE = 0, PAPP_GPIO_PULL_UP = 1, PAPP_GPIO_PULL_DOWN = 2 };
enum {                                       /* gpio_attach trigger                           */
    PAPP_IRQ_RISING = 1, PAPP_IRQ_FALLING = 2, PAPP_IRQ_ANY_EDGE = 3,
    PAPP_IRQ_LOW = 4, PAPP_IRQ_HIGH = 5,
};

/* On v1.x chips an interrupt that finds its core idle (no task of the app's or the launcher's running there)
 * starts some 17 us late: a pin edge reaches the handler in ~20 us (up to ~70) on an idle core, ~3 us on a
 * busy one (v3.x chips: ~3 us either way).
 * PAPP_IRQ_AWAKE keeps the core busy while this interrupt is attached (a launcher task spinning there at
 * the lowest priority, below every app thread): ~3 us always, for that core's idle time and power. */
#define PAPP_IRQ_AWAKE  (1u << 0)

typedef struct papp_irq papp_irq_t;

typedef struct {
    int64_t  time_us;                        /* papp_time_us() clock, taken in the interrupt   */
    uint32_t count;                          /* interrupts of this handle so far (1 = first)   */
    uint16_t dropped;                        /* events lost before this one (queue was full)   */
    uint8_t  pin;                            /* the GPIO, or 0xFF for a timer                  */
    uint8_t  level;                          /* the pin's level read in the interrupt          */
} papp_irq_event_t;

/* What a direct handler may use (in internal RAM, valid during the call). */
typedef struct {
    const papp_irq_event_t *ev;              /* this interrupt                                 */
    papp_irq_t *self;
    void (*gpio_write)(int pin, int level);  /* pins the app set up as outputs                 */
    int  (*gpio_read)(int pin);
    void (*signal)(papp_irq_t *h, const papp_irq_event_t *ev);   /* queue ev for wait() on h   */
    void (*timer_arm)(papp_irq_t *timer, uint32_t us);   /* (re)start a one-shot timer in us   */
} papp_irq_ctx_t;

typedef void (*papp_irq_isr_t)(void *arg, const papp_irq_ctx_t *ctx);

typedef struct {
    int8_t          core;                    /* 0 or 1: where the interrupt runs; -1 = 1     */
    uint8_t         priority;                /* 1..3 (3 = highest); 0 = 1                     */
    uint8_t         queue;                   /* events kept for wait(); 0 = 16, at most 64    */
    uint8_t         flags;                   /* PAPP_IRQ_AWAKE                                */
    papp_irq_isr_t  isr;                     /* NULL = deferred; else direct (PAPP_FAST)      */
    void           *isr_arg;
} papp_irq_cfg_t;

typedef struct {
    papp_iface_hdr_t hdr;
    uint64_t (*gpio_mask)(void);             /* bit n = GPIO n is free for apps on this board  */
    int  (*gpio_config)(int pin, int mode, int pull);    /* PAPP_E_OK, ARG (not a free pin)    */
    int  (*gpio_write)(int pin, int level);
    int  (*gpio_read)(int pin);              /* 0/1, or PAPP_E_ARG                             */
    /* An interrupt on a pin set up with gpio_config (an output pin works too: it sees its own edges).
     * Starts enabled. PAPP_E_LIMIT: PAPP_IRQ_MAX_PINS reached or the pin has one already. */
    int  (*gpio_attach)(int pin, int trigger, const papp_irq_cfg_t *cfg, papp_irq_t **out);
    /* A hardware timer with 1 us resolution, stopped until timer_start. PAPP_E_LIMIT: none free. */
    int  (*timer_attach)(const papp_irq_cfg_t *cfg, papp_irq_t **out);
    int  (*timer_start)(papp_irq_t *t, uint32_t us, bool periodic);   /* first interrupt in us  */
    int  (*timer_stop)(papp_irq_t *t);
    int  (*enable)(papp_irq_t *h, bool on);  /* a pin's interrupt; a timer: same as stop/start  */
    /* The next queued event: 1 = *ev filled, PAPP_E_TIMEOUT, PAPP_E_EXIT (the app is asked to
     * exit, or h was detached). */
    int  (*wait)(papp_irq_t *h, papp_irq_event_t *ev, uint32_t timeout_ms);
    void (*detach)(papp_irq_t *h);           /* interrupt off and freed; a waiter gets E_EXIT  */
    int  (*core)(papp_irq_t *h);             /* the core its interrupt runs on                 */
} papp_if_irq_t;

/* ── CODC interface ─────────────────────────────────────────────────────── */
/* Decoders for video and audio, and the P4's hardware JPEG engine.
 *
 * Decoders (open / send / receive / close) work on a container's packets: the app reads the file (MP4, MKV, MP3,
 * ADTS ...) and hands each packet over; what comes out is a YUV 4:2:0 picture (for DISP v5 blit_yuv) or 16-bit
 * PCM (for AUDI). The launcher picks the implementation: hardware where the chip has it (JPEG; a future chip's
 * H.264 decoder would slot in here with no change to apps), else software built into the launcher (MP3, AAC) or
 * a codec module loaded from the SD card on first use (/sd/launcher/codecs/<codec>.papp, e.g. h264.papp: FFmpeg's
 * decoder). PAPP_E_UNSUPPORTED from open = no decoder for it here (a missing module: the Store installs them).
 * Everything an app opened is closed when it ends, and modules are unloaded then.
 *
 * Cores: cfg.cores = 2 lets a video decoder work on two frames at once on both cores (about 1.7x faster for
 * H.264, one frame more delay); 1 keeps it on one thread; 0 = the decoder's choice (2 for video). Its threads
 * run at cfg.prio (PAPP_PRIO_*).
 *
 * The JPEG calls: the hardware engine, RGB565 pictures in the app's own buffers (the launcher copies through
 * DMA buffers of its own). One picture at a time across the app's threads (a second caller waits). */

#define PAPP_IF_CODC      PAPP_FOURCC('C', 'O', 'D', 'C')
#define PAPP_IF_CODC_VER  1

#define PAPP_E_EOF        (-15)          /* receive: the stream has ended (after send(NULL)), all output given */
#define PAPP_PTS_NONE     INT64_MIN      /* an output with no pts of its own (see receive_audio)               */

enum {                                   /* papp_dec_cfg_t.codec                                         */
                                         /* 1: not used (JPEG pictures: jpeg_decode, the hardware)       */
    PAPP_CODEC_H264  = 2,                /* video: Annex B (start codes), or AVC packets with .extra = the
                                            MP4's avcC box                                               */
    PAPP_CODEC_MP3   = 3,                /* audio: MPEG-1/2 layer III frames (any split: the decoder finds
                                            the frames)                                                  */
    PAPP_CODEC_AAC   = 4,                /* audio: ADTS, or raw frames with .extra = the MP4's
                                            AudioSpecificConfig                                          */
};

typedef struct {
    uint8_t     codec;                   /* PAPP_CODEC_*                                                  */
    uint8_t     cores;                   /* 0 = the decoder's choice, 1, 2                                */
    uint8_t     prio;                    /* PAPP_PRIO_* of the decoder's threads                          */
    uint8_t     flags;                   /* 0                                                             */
    const void *extra;                   /* codec set-up from the container (copied), or NULL             */
    uint32_t    extra_len;
    uint32_t    reserved[4];             /* 0 */
} papp_dec_cfg_t;

typedef struct {                         /* a decoded picture: the decoder's memory, valid until the next
                                            receive_frame, flush or close                                */
    const uint8_t *y, *u, *v;            /* planar YUV 4:2:0                                              */
    uint16_t    w, h;
    uint16_t    stride_y, stride_uv;
    int64_t     pts;                     /* as sent with its packet                                       */
    uint8_t     yuv_flags;               /* PAPP_YUV_*: pass to blit_yuv                                  */
    uint8_t     keyframe;
    uint8_t     reserved8[2];
    uint32_t    reserved[2];
} papp_dec_frame_t;

typedef struct {
    int64_t     pts;
    uint32_t    rate;                    /* Hz                                                            */
    uint8_t     channels;                /* 1, 2: the PCM is interleaved                                  */
    uint8_t     reserved8[3];
    uint32_t    reserved[2];
} papp_dec_audio_t;

#define PAPP_DEC_AUDIO_MAX  4608         /* samples per channel one receive_audio can give: room for this */

typedef struct {
    uint8_t     available;               /* open would work now                                           */
    uint8_t     hardware;                /* done by the chip                                              */
    uint8_t     module;                  /* a codec module from the SD card                               */
    uint8_t     max_cores;               /* 0: not known before its module is loaded                      */
    char        name[40];                /* e.g. "FFmpeg 7.1.1 h264", "esp_audio_codec 2.5.0 mp3"         */
} papp_codec_info_t;

typedef struct papp_dec papp_dec_t;

typedef struct {
    papp_iface_hdr_t hdr;
    /* A JPEG's size from its header: PAPP_E_OK, or PAPP_E_ARG (not a JPEG the engine takes:
     * progressive and arithmetic-coded JPEGs are not). */
    int (*jpeg_info)(const void *jpg, size_t len, uint16_t *w, uint16_t *h);
    /* Decodes into out: RGB565, the picture's w x h, stride_px pixels per line (0 = its width),
     * out_size bytes at most. *w / *h (may be NULL) get the size. PAPP_E_OK, PAPP_E_ARG, PAPP_E_LIMIT
     * (out too small), PAPP_E_NOMEM, PAPP_E_IO (a damaged picture), PAPP_E_UNSUPPORTED (width or height
     * not a multiple of 8: the engine cannot; decode those in software). */
    int (*jpeg_decode)(const void *jpg, size_t len, uint16_t *out, int stride_px, size_t out_size,
                       uint16_t *w, uint16_t *h);
    /* Encodes an RGB565 picture (w x h, stride_px, 0 = w) at quality 1..100 (0 = 80) into out:
     * returns the JPEG's length in bytes, or PAPP_E_ARG, PAPP_E_LIMIT (out too small), PAPP_E_NOMEM,
     * PAPP_E_IO. */
    int (*jpeg_encode)(const uint16_t *pixels, int w, int h, int stride_px, int quality, void *out,
                       size_t out_size);

    /* What decodes codec here (loads nothing). PAPP_E_OK, PAPP_E_ARG (unknown codec). */
    int (*codec_info)(int codec, papp_codec_info_t *out);
    /* A decoder (loads its module on first use, which takes a moment). PAPP_E_OK, PAPP_E_UNSUPPORTED,
     * PAPP_E_ARG (bad extra), PAPP_E_NOMEM, PAPP_E_LIMIT (8 decoders open already). */
    int (*open)(const papp_dec_cfg_t *cfg, papp_dec_t **out);
    /* One packet (copied; pts is handed back with its output). data NULL = the end of the stream: what is
     * left comes out, then receive answers PAPP_E_EOF. PAPP_E_OK, PAPP_E_LIMIT (output is waiting: receive
     * it, then send this packet again), PAPP_E_IO (damaged: dropped, carry on). */
    int (*send)(papp_dec_t *d, const void *data, size_t len, int64_t pts);
    /* The next picture: 1 = *f filled, 0 = none yet (send more), PAPP_E_EOF, PAPP_E_ARG (an audio decoder). */
    int (*receive_frame)(papp_dec_t *d, papp_dec_frame_t *f);
    /* The next audio: samples per channel (> 0) into pcm, 16-bit, interleaved when stereo: pcm has room for
     * max samples x 2 channels (max = PAPP_DEC_AUDIO_MAX is always enough). 0 = none yet (send more),
     * PAPP_E_EOF, PAPP_E_LIMIT (max too small), PAPP_E_ARG (a video decoder). info->pts = the pts of the
     * packet it came from; when one packet gives several, the first has it and the rest PAPP_PTS_NONE
     * (count the samples). */
    int (*receive_audio)(papp_dec_t *d, int16_t *pcm, size_t max, papp_dec_audio_t *info);
    /* Forget what is in flight (after a seek: send a keyframe next). A decoder is used by one thread at a
     * time (its own threads, cores = 2, are the decoder's business). */
    void (*flush)(papp_dec_t *d);
    void (*close)(papp_dec_t *d);
} papp_if_codc_t;

/* A codec module (/sd/launcher/codecs/<codec>.papp, built with papp_build --module): a PAPP image whose entry
 * hands the launcher this table. It runs in the app's run (its memory and threads are the app's, through the
 * same interfaces) and is unloaded when the app ends. Its functions have the CODC decoder's meanings. */
#define PAPP_CODEC_MODULE_MAGIC  PAPP_FOURCC('C', 'M', 'O', 'D')
#define PAPP_CODEC_MODULE_VER    1
typedef struct {
    uint32_t    magic;                   /* PAPP_CODEC_MODULE_MAGIC                                      */
    uint16_t    version;                 /* PAPP_CODEC_MODULE_VER                                        */
    uint8_t     max_cores;
    uint8_t     reserved;
    const char *name;
    bool (*supports)(int codec);
    int  (*open)(const papp_dec_cfg_t *cfg, void **ctx);
    int  (*send)(void *ctx, const void *data, size_t len, int64_t pts);
    int  (*receive_frame)(void *ctx, papp_dec_frame_t *f);
    int  (*receive_audio)(void *ctx, int16_t *pcm, size_t max, papp_dec_audio_t *info);
    void (*flush)(void *ctx);
    void (*close)(void *ctx);
} papp_codec_module_t;
/* The launcher calls a module's entry as _papp_start(PAPP_MODULE_ARGC, argv) with argv[0] pointing at a
 * `const papp_codec_module_t *` for it to fill; it returns 0. */
#define PAPP_MODULE_ARGC  (-0x4D4F44)

/* ── Entry point ────────────────────────────────────────────────────────── */

/* Provided by the SDK runtime (crt0), located at entry_off. */
typedef int (*papp_start_fn_t)(int argc, char **argv);

#ifdef __cplusplus
}
#endif

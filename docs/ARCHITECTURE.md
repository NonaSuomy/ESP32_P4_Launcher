# ESP32-P4 Launcher — Proposed Architecture

> Status: **proposal / draft v0.1** (2026-10-29). Derived from a review of the PAPP implementation in
> `C:\ESPIDFprojects\RetroESP32_P4` (`components/psram_app_loader`, `tools/psram_app.ld`,
> `tools/pack_papp.py`, `PAPP_GUIDE.md`, `PSRAM_APP.md`, `apps/psram_*`).
> Companion document: [`WORKPLAN.md`](WORKPLAN.md).

---

## 1. Goals

1. **Run applications straight from the SD card** — load, relocate, execute, return to the launcher.
   No flash partition, no reboot, no flash wear.
2. **A DLL-style service layer** — the launcher exports display (with scaling/rotation), audio, input
   (gamepads, keyboards, touch), memory, files, threads, PPA/2D acceleration, settings and UI dialogs.
3. **Binary portability** — the *same* `.papp` file runs unmodified on every ESP32-P4 board that has
   the launcher. Only the launcher is ported.
4. **Robustness** — an app can't leave the system broken: every resource it took is reclaimed on exit,
   forced quit always works, crashes are reported instead of mysterious reboots.
5. **Performance good enough to host emulators** — so that, eventually, the OTA "one firmware per
   emulator + reboot" model is no longer needed.

First target: the reference board (Guition ESP32-P4 4.3″ 480×800 MIPI-DSI + GT911 + ES8311), plus its
HDMI variant (LT8912 bridge) as a second board configuration.

---

## 2. What we keep from the reference PAPP, and what we change

The reference implementation proved the hard part: **PSRAM execute-in-place on the P4 works**
(page-aligned PSRAM buffer → D-cache writeback → second `esp_mmu_map(EXEC|READ|32BIT)` mapping →
I-cache invalidate → call). Four large engines (Doom, Quake, Duke3D, OpenTyrian) and LVGL run that
way. We keep that mechanism. What needs to change is everything around it:

| # | Limitation in the reference design | Consequence | New design |
|---|---|---|---|
| 1 | Apps are linked at a **fixed address `0x4A000000`** and the loader *hopes* `esp_mmu_map()` returns it | Breaks as soon as the launcher maps anything else first, or on a board with a different PSRAM size; can't place code in internal SRAM | **Relocatable images**: link with `--emit-relocs`, host tool emits a compact relocation table, loader patches at any base (§5) |
| 2 | **One flat `app_services_t`** of ~45 fn pointers, append-only, `NULL`-checks for anything new | Silent `NULL` call = crash; can't version a subsystem independently; every board must fake every service | **Named, individually versioned interfaces** imported by the app at load time, with required/optional semantics (§6) |
| 3 | **Board details leak into the ABI**: 800×480 native FB, "rotate 270°" baked in, touch in 800×480 landscape space, `png_load` swaps R↔B "for MIPI", `MALLOC_CAP_*` values copied verbatim, input enum = odroid layout | An app written for this board is wrong on any other board (the guide already notes HDMI is 640×480 and there's no "get FB size") | **Logical surfaces**: app declares its canvas; the launcher compositor owns rotation/scale/letterbox/colour-order; touch arrives in canvas coordinates (§7.3) |
| 4 | **Apps drive hardware directly** through thin wrappers (`audio_init` reconfigures I2S, app audio task blocks inside `i2s_channel_write`) | The documented I2S-mutex deadlock on the next launch; per-app audio rate juggling | **Launcher owns every driver.** Apps talk to queues/ring buffers. Audio is mixed and resampled by the launcher (§7.4) |
| 5 | **No resource ownership**: plain `malloc`, raw `xTaskCreate`, raw `FILE*` | 155–220 KB leaked per run; forced exit impossible; app must clean up perfectly | **App context with arenas + resource tracker**; everything reclaimed in one shot (§8) |
| 6 | **Every port re-implements libc glue** (`papp_syscalls.c` ×4 ≈ 1 700 lines; malloc `--wrap`, fd tables, `_fstat` bug fixed per app) | Bugs fixed in one app and not the others; high porting cost | **SDK runtime library** (`libpapp_rt.a`): crt0, constructors, newlib syscalls, malloc routing, retarget locks, stdio — written once (§9) |
| 7 | **Manual `.bss` sizing** (`--bss-size` computed in each build script; forgetting it corrupts the heap) | Easy-to-make, hard-to-debug corruption | Packer reads section sizes from the ELF; impossible to get wrong (§5) |
| 8 | No metadata: no name/version/icon/file-associations; no integrity check | Launcher can only list files; no "open `.nes` with NES app" | **Manifest** embedded in the `.papp` + CRC (§5.2) |
| 9 | Code runs only from PSRAM (through 16 KB L1 I$ + 128 KB L2) | Hot emulator loops are slower than in the OTA builds (which pin hot paths to IRAM) | Optional **FAST region in internal SRAM** for `.fast.*` code/data (§5.3) |
| 10 | Exit = engine-specific (setjmp/longjmp, MENU-hold watchdog task per app) | Inconsistent UX, duplicated code | Launcher **overlay menu** + lifecycle events (PAUSE/RESUME/EXIT_REQUEST) + forced quit (§8.2) |

---

## 3. Layered architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  PAPP applications  (/sd/apps/<id>/<id>.papp)       — board-agnostic, binary  │
│  ┌──────────────────────┐  ┌──────────────────────────────────────────────┐  │
│  │ app code (C / C++)   │  │ papp SDK runtime (static, linked into app):  │  │
│  │ + optional libs      │  │ crt0 · libc glue · interface stubs · SDL-lite│  │
│  └──────────────────────┘  └──────────────────────────────────────────────┘  │
╞═════════════════════════  PAPP ABI v2 (interfaces)  ═════════════════════════╡
│  LAUNCHER CORE  (board-agnostic, ESP-IDF application in the factory slot)   │
│  ┌────────────┐ ┌──────────────┐ ┌────────────┐ ┌───────────┐ ┌───────────┐  │
│  │ Loader /   │ │ App Manager  │ │ Service    │ │ Launcher  │ │ Dev tools │  │
│  │ Relocator  │ │ lifecycle,   │ │ registry   │ │ UI (LVGL) │ │ upload/run│  │
│  │ + importer │ │ res. tracker │ │ (exports)  │ │ + overlay │ │ log/crash │  │
│  └────────────┘ └──────────────┘ └────────────┘ └───────────┘ └───────────┘  │
│  ┌───────────┐ ┌──────────┐ ┌───────────┐ ┌────────┐ ┌──────┐ ┌───────────┐  │
│  │Compositor │ │Audio     │ │Input mgr  │ │Storage │ │Memory│ │Gfx accel  │  │
│  │surfaces,  │ │mixer,    │ │gamepad DB,│ │sandbox │ │arenas│ │PPA, 2D-DMA│  │
│  │PPA scale  │ │resampler │ │HID kbd,   │ │paths   │ │TLSF  │ │JPEG, PNG  │  │
│  └───────────┘ └──────────┘ │touch xform│ └────────┘ └──────┘ └───────────┘  │
│                             └───────────┘                                    │
╞═════════════════════════  BSP interface (bsp_*.h)  ══════════════════════════╡
│  BOARD SUPPORT PACKAGE — one component per board (boards/<board_id>/)        │
│  panel + backlight · touch + transform · codec + amp · buttons/ADC/expander  │
│  USB host port · SD slot · battery/power · LEDs · capability descriptor      │
╞══════════════════════════════════════════════════════════════════════════════╡
│  ESP-IDF 5.5.x · FreeRTOS · esp_lcd · PPA · 2D-DMA · JPEG · I2S · USB host   │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Rules that make portability work**

- An app only ever sees the ABI. It never includes an ESP-IDF header and never sees a board name.
- The launcher core only ever sees `bsp_*.h`. No `#ifdef CONFIG_HDMI_OUTPUT` in the core (today's HDMI
  build is just another board config: `guition_p4_hdmi`).
- Capabilities are *queried*, not assumed: `papp_sys_board_caps()` says whether there's touch, a
  paddle, a keyboard, a battery, how big the screen is.

---

## 4. Execution model

```
            core 0                                   core 1
 ┌────────────────────────────┐        ┌──────────────────────────────────┐
 │ papp_main task (app)       │        │ compositor task  (PPA, flip)     │
 │  stack: manifest-declared  │ queue  │ audio task (mix → resample → I2S)│
 │  (SRAM ≤32 KB, else PSRAM) │◀──────▶│ input task (USB HID, GPIO, touch)│
 │ + app threads (tracked)    │        │ launcher service task (overlay,  │
 └────────────────────────────┘        │   dev tools, crash/heartbeat)    │
                                       └──────────────────────────────────┘
```

- While an app runs, the launcher UI is **suspended** (its LVGL objects freed or parked in PSRAM),
  but the system tasks on core 1 keep running. They are the only code that touches drivers.
- Apps may create threads on either core; priorities are abstract classes
  (`LOW/NORMAL/HIGH/REALTIME`) mapped by the launcher so apps can't starve system tasks.
- One app at a time (no multitasking). This keeps the resource tracker trivial: "the current app"
  owns everything allocated through the ABI.

---

## 5. The `.papp` v2 image format

### 5.1 Build pipeline

```
 .c/.cpp ──gcc──▶ .o ──ld (-T papp.ld, -Wl,-q, base 0)──▶ app.elf ──papp-pack──▶ app.papp
                                     ▲                                    │
                       libpapp_rt.a (crt0, libc glue, stubs)              ├─ section images
                                                                          ├─ relocation table
                                                                          ├─ import table
                                                                          ├─ manifest
                                                                          └─ CRC32
```

Compile flags stay as today (`-march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany`,
`-ffunction-sections -fdata-sections`, `--no-relax`), plus `-Wl,-q` (`--emit-relocs`) so the final
ELF still contains its relocations. `papp-pack` (Python, host side) reads the ELF and produces the
image — section sizes come from the ELF, so the `.bss` sizing bug class disappears.

### 5.2 Container layout

```
 offset  content
 0x000   papp_hdr_v2_t (64 B): magic "PAP2", fmt_version, flags, header_crc,
         region table offsets, manifest/import/reloc offsets+counts, image_crc32
 ...     manifest   (TLV): id, name, version, author, sdk_version, min_abi,
                           main_stack (size + SRAM|PSRAM), heap hints,
                           canvas preference, required caps (touch/keyboard/…),
                           file associations (".nes,.fds"), icon path, flags
 ...     imports    : [{ iface_id (FourCC), min_version, flags REQUIRED|OPTIONAL,
                         slot_offset (where to write the vtable ptr) }]
 ...     relocs     : packed list of { type, region, offset } (see 5.4)
 ...     region SLOW  (PSRAM, executable mapping):  .text .rodata .data   (+ .bss size)
 ...     region FAST  (internal SRAM, optional):    .fast.text .fast.data (+ .fast.bss size)
```

Icons/resources stay as files in the app folder (`/sd/apps/<id>/icon.png`) so they're editable without
rebuilding.

### 5.3 Memory regions

| Region | Physical memory | Contents | Mapping |
|---|---|---|---|
| **SLOW** | PSRAM | all code + data by default | page-aligned buffer, D-cache writeback, second EXEC mapping (today's proven path) |
| **FAST** | internal SRAM (L2MEM) | functions/data the app tags `PAPP_FAST` / `PAPP_FAST_DATA` | executable SRAM arena reserved by the launcher at boot |

The FAST region is what makes emulators viable as PAPPs (the OTA builds already depend on putting
hot loops in IRAM). It requires executable internal RAM: the reference launcher has
`CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=y`, which makes heap SRAM non-executable. Options, to be decided by
**Spike B** in the work plan: (a) disable the split and reserve an RWX arena; (b) keep the split and
reserve a region via a custom linker fragment + PMP entry. FAST is optional per app; a board/launcher
without it simply loads FAST sections into SLOW (correct, just slower).

**Implemented (launcher 0.6.0):** option (a). Apps tag code `PAPP_FAST` and data `PAPP_FAST_DATA` /
`PAPP_FAST_BSS`; `papp.ld` links FAST at `0x10000000` (so a short jump across regions is a link error);
the packer stores the FAST image after SLOW and emits region-tagged relocations, including
`PCREL_HI20/LO12` for cross-region calls and address loads and `DIFF32` for switch jump tables whose
labels are in FAST while the table is in SLOW `.rodata`. Note that on the P4 internal SRAM is also
reached through the L1 caches, so the loader writes back / invalidates it like PSRAM.

L2 cache size (`CONFIG_CACHE_L2_CACHE_128KB` today) trades SRAM for PSRAM-XIP speed; benchmark 128 vs
256 KB in Spike B.

### 5.4 Relocation

Because the two regions land at independent addresses, the loader must patch:

| Type | When | Patch |
|---|---|---|
| `ABS32` | absolute pointers in data (vtables, string tables, function-pointer arrays, `.init_array`) | `*p += base(target_region)` |
| `PCREL_HI20 / LO12_I / LO12_S` pair | `auipc`-based reference **crossing regions** (SLOW↔FAST) | recompute `target - pc`, re-encode the instruction pair |
| `CALL` (auipc+jalr) | cross-region calls | same as above |

References *within* a region are PC-relative under `medany` and need no patching — only cross-region
pairs and absolute words. `papp-pack` resolves this from the ELF `.rela.*` sections and rejects
anything it can't express (e.g. a `JAL`/branch crossing regions → build error with a clear message).
Espressif's `elf_loader` component (esp-iot-solution) does on-device ELF relocation for RISC-V and is a
useful reference, but we pre-process on the host to keep the loader small and fast.

### 5.5 Load sequence

1. Read + validate header (magic, format version, header CRC). Read manifest.
2. **Compatibility check before allocating anything**: every `REQUIRED` import must exist with
   `version >= min_version`; required capabilities must be present. Otherwise show
   *"Doom needs AUDIO v2 — this launcher has v1"* and stop.
3. Create the app context (§8): PSRAM arena, SRAM arena, resource table.
4. Allocate SLOW (64 KB-page aligned) and FAST buffers; stream the region images from SD; zero `.bss`.
5. Apply relocations; write interface pointers into import slots.
6. D-cache writeback; `esp_mmu_map(EXEC)`; I-cache invalidate; verify image CRC.
7. Create `papp_main` task with the manifest stack; call `crt0` entry → constructors → `app_main()`.
8. On exit: run destructors, then the App Manager tears down the context (§8).

---

## 6. ABI v2: interfaces ("DLL" model)

### 6.1 Mechanism

Each service is an **interface**: a struct whose first fields are `{ uint32_t id; uint16_t version;
uint16_t size; }` followed by function pointers. The launcher keeps a registry of the interfaces it
exports. The app's import table lists what it needs; the loader writes the pointers into slots in the
app's `.data`. The SDK hides all of this behind ordinary C functions:

```c
/* papp/audio.h — what an app developer sees */
papp_audio_stream_t *papp_audio_open(const papp_audio_cfg_t *cfg);
int  papp_audio_write(papp_audio_stream_t *s, const int16_t *frames, int n, int timeout_ms);
void papp_audio_close(papp_audio_stream_t *s);

/* libpapp_rt — generated stub */
extern const papp_if_audio_t *__papp_imp_AUDI;          /* filled by the loader */
int papp_audio_write(papp_audio_stream_t *s, const int16_t *f, int n, int t)
{ return __papp_imp_AUDI->write(s, f, n, t); }
```

Versioning rules:

- **Minor evolution** = append functions to an interface and bump `version`. Old apps keep working.
  New apps that *require* the new function declare `min_version`; the loader refuses instead of
  crashing. Apps that merely *prefer* it check `papp_has(AUDI, 3)` — the SDK checks `size`.
- **Breaking change** = a new interface ID (`AUD2`). The launcher may export both for a while.
- A board without a feature simply doesn't export the interface (e.g. no `TUCH` on an HDMI box);
  apps that marked it `OPTIONAL` adapt, apps that marked it `REQUIRED` are greyed out in the UI.

This gives DLL-like behaviour (link-by-name, load-time resolution, clear errors) with
function-pointer-table speed and no symbol strings on the device.

### 6.2 Interface catalogue (initial)

| ID | Interface | Key functions | Notes |
|---|---|---|---|
| `SYS ` | System | abi/launcher version, `board_caps`, log, time_us, sleep, yield, exit, lifecycle event poll, TRNG, perf counters | always present |
| `MEM ` | Memory | `alloc(size, caps, align)`, `realloc`, `free`, `stats` | caps are **abstract**: `FAST` (internal), `DMA`, `LARGE` (PSRAM); v2 (launcher 0.29.1) `code_alloc` / `code_free` / `code_sync`: executable PSRAM for JITs (64 KB-aligned block mapped a second time with EXEC; the app writes code through the data address and runs it at the returned one after `code_sync` = cache write-back + I-cache invalidate + `fence.i`); since 0.29.2 `code_sync` also takes executable internal SRAM (the app's FAST region, e.g. a JIT's hot code buffer; older launchers return `PAPP_E_ARG`); leftover blocks are freed at exit |
| `THRD` | Threads & sync | thread create/join/exit (stack size + location, prio class, core), mutex, recursive mutex, semaphore, queue, event flags, timers | all tracked; v1 (launcher 0.5.0) has threads, mutexes, semaphores, queues; event flags + timers to come |
| `FS  ` | Files | open/read/write/seek/tell/close/stat/truncate, opendir/readdir, mkdir/remove/rename, `app_dir()`, `data_dir()` | sandboxed paths (§7.6) |
| `DISP` | Display / compositor | `info()`, surface create/destroy, `lock_back_buffer`, `present(async, vsync)`, palette for 8bpp, scale mode, dirty rects, direct-native mode; v3 `present_ext` (app's own buffer; since launcher 0.29.1 copied into the surface's spare buffer); since 0.29.1 a present on a 2-buffer surface returns at once and a launcher task scales and flips while the app draws its next frame (1-buffer surfaces stay synchronous); v4 `blit` (PPA into the surface: RGB565 / ARGB8888 / A8 pictures, scaled in 1/16 steps, quarter turns, mirrors via an SRM pass into a launcher buffer, then blended with colour key and alpha) and `fill` (PPA fill); v5 (launcher 0.37) `blit_yuv` (planar YUV 4:2:0 video frame -> RGB565, scaled, by the PPA; §7.11) | §7.3 |
| `GFX ` | 2D acceleration | PPA SRM (scale/rotate/mirror/convert), PPA blend (alpha, colour key), PPA fill, 2D-DMA copy, async + sync | generalises `sprite_blit` / `fb_copy` |
| `CODC` | Codecs | Decoders: open / send / receive / flush / close on a container's packets, out comes YUV 4:2:0 (for `blit_yuv`) or 16-bit PCM; MP3 + AAC built in (esp_audio_codec 2.5.0), H.264 = codec module from the SD card (`/sd/launcher/codecs/h264.papp`, FFmpeg), cores 1 or 2; JPEG info / decode / encode in RGB565 on the P4's JPEG engine | v1 (launcher 0.37); §7.11; H.264 hardware encode later |
| `IMG ` | Image codecs | PNG decode | not planned now (JPEG is in `CODC`) |
| `AUDI` | Audio | stream open (rate, channels, format, latency), write/push, pull callback, pause, `get_queued` | launcher mixes + resamples |
| `INPT` | Input | gamepad state (player N, standard layout + analog axes + triggers), keyboard state, event queue (key/text/button/axis/touch/mouse/device-added), rumble (reserved) | §7.5 |
| `KV  ` | Settings | per-app key/value (int, string, blob); read-only global prefs (volume, brightness, language) | replaces NVS-path hacks |
| `UI  ` | Launcher dialogs | **file picker** (extension filter, start dir), message box, menu, volume, **Wi-Fi picker** (v4: the network chosen becomes the board's saved one; the app never sees the password), on-screen keyboard, progress, toast | rendered by the launcher on the overlay layer |
| `PWR ` | Power / board | battery %, charging, backlight, LED | optional |
| `NET ` | Network | Wi-Fi (up with the saved network, info, scan, join for the run, SoftAP, events), DNS, TCP/UDP sockets with poll, TLS client, HTTP/1.1 client | v1 (launcher 0.29); the first call keeps Wi-Fi on for the run; `docs/NET_BLE_PROPOSAL.md` |
| `BLE ` | Bluetooth LE | scan, connect, GATT client (discover, read, write, subscribe), GATT server (one table, values kept by the launcher, notify/indicate), advertising, events | v1 (launcher 0.29), Guition boards and Tab5; the stack (Bluedroid since 0.35, NimBLE before) runs only while used; §7.10; `docs/NET_BLE_PROPOSAL.md` |
| `IRQ ` | Pins, timers, interrupts | free-pin mask, pin config/read/write, pin and timer interrupts on a chosen core, deferred (`wait`) or direct (app's FAST function in the interrupt) | v1 (launcher 0.35); §7.9 |
| `BTHD`, `USBD`, `CAM ` | Reserved | BLE HID, USB device/MSC, MIPI-CSI camera | later phases |

---

## 7. Launcher services in detail

### 7.1 Board Support Package (BSP) interface

```c
typedef struct {
    const char *id;                    /* "guition_p4_43", "guition_p4_hdmi", ... */
    uint32_t    caps;                  /* BSP_CAP_TOUCH | BSP_CAP_PADDLE | ...     */
    bsp_display_info_t display;        /* w, h, native rotation, colour order, refresh, interface */
    bsp_touch_xform_t  touch;          /* raw → panel transform (swap/mirror/scale)               */
    bsp_audio_info_t   audio;          /* supported rates, has speaker/headphone detect           */
    bsp_input_map_t    buttons;        /* GPIO/ADC/expander buttons → logical buttons             */
    size_t psram_size, launcher_psram_reserve, fast_arena_size;
} bsp_board_desc_t;

esp_err_t bsp_init(void);
esp_err_t bsp_display_init(bsp_fb_t *fbs, int n_fbs);   /* returns DMA framebuffers */
void      bsp_display_flip(int index);                  /* + vsync callback */
void      bsp_backlight_set(int pct);
bool      bsp_touch_read_raw(bsp_touch_point_t *pts, int max);
esp_err_t bsp_audio_init(int rate);  int bsp_audio_write(const int16_t *, size_t, int timeout);
void      bsp_audio_volume(int pct);
uint32_t  bsp_buttons_read(void);    int bsp_analog_read(int ch);
esp_err_t bsp_sdcard_mount(const char *mount_point);
int       bsp_battery_pct(void);
```

Each board lives in `boards/<board_id>/` (a component selected by Kconfig). Where Espressif publishes a
BSP (e.g. `esp32_p4_function_ev_board`), our board component wraps it. Drivers from the reference
project (`st7701_lcd`, `gt911_touch`, ES8311 in `odroid_audio`, `lt8912`, `odroid_input` incl. the
stuck-pin detection, `usb_host_hid`/`gamepad`) move into the reference board package.

### 7.2 Memory layout (reference board: 32 MB PSRAM, 768 KB SRAM)

| Area | Size (initial guess, Kconfig per board) | Owner |
|---|---|---|
| Launcher PSRAM | ~4 MB (2–3 framebuffers, LVGL pool, icons, audio buffers) | launcher |
| **App PSRAM arena** | rest (~27 MB): SLOW image + app heap (TLSF) | current app |
| Launcher SRAM | IDF + drivers + system task stacks + DMA buffers | launcher |
| **App FAST arena** | 128–256 KB internal SRAM (executable, see Spike B) | current app |
| L2 cache | 128 KB (benchmark 256 KB) | hardware |

Arenas are created per app run and destroyed wholesale on exit → **zero leaks by construction**.

### 7.3 Display compositor

The app never sees the panel. It creates one (or more) **surfaces**:

```c
papp_surface_cfg_t cfg = {
    .width = 320, .height = 240, .format = PAPP_PIXFMT_RGB565,   /* or L8 + palette, RGB888 */
    .scale = PAPP_SCALE_FIT_ASPECT,      /* FIT_ASPECT | INTEGER | STRETCH | NONE (1:1, centred) */
    .filter = PAPP_FILTER_BILINEAR,
    .buffers = 2,                        /* double / triple buffering */
};
papp_surface_t *s = papp_disp_surface_create(&cfg);
uint16_t *px = papp_disp_lock(s);  /* draw */  papp_disp_present(s, PAPP_PRESENT_VSYNC);
```

The compositor (core 1) computes the transform once per surface — panel rotation from the BSP
(portrait 480×800 LCD → rotate 270°; HDMI 640×480 → none), scale factor, letterbox offsets, colour
order — and does it with one PPA SRM operation per frame into the panel framebuffer, then flips. It
also composites the **launcher overlay layer** (volume OSD, FPS, in-app menu, dialogs) with PPA blend.

Consequences: the 400×240 template, Doom's 320×200 ×2.4, Quake's 320×240 ×2.0 all become one line
(`FIT_ASPECT`) and look right on any panel. `papp_disp_info()` exposes the physical size for apps that
want native resolution (`PAPP_SCALE_NONE` with a surface the size of the display).

**Touch** is delivered **in surface coordinates** (the compositor knows the inverse transform), so the
`canvas = native / 2` and "landscape X = portrait Y" hacks go away.

8-bit indexed surfaces: the compositor converts L8→RGB565 (optimised with the P4 PIE SIMD
extension, or PPA if/where it accepts CLUT input) — every engine port today does this by hand.

### 7.4 Audio engine

- The codec runs at **one fixed rate** chosen by the BSP (e.g. 48 kHz or 44.1 kHz). I2S is configured
  once at boot and never reconfigured → the "I2S mutex deadlock on second launch" bug class is gone.
- Apps open N **streams** at any rate/format (11 025 Hz mono 8-bit … 48 kHz stereo 16-bit). Each stream
  has a launcher-owned ring buffer; the audio task pulls, resamples, mixes, applies master volume,
  writes to I2S.
- Two models: **push** (`papp_audio_write`, blocks with timeout — matches emulators) and **pull**
  (callback on the audio task — matches SDL-style engines).
- Underrun → silence, never a stall; stream stats (`queued`, `underruns`) are exposed for emulator
  rate-control (the per-emulator audio strategies of the reference Phase 48 become app-side policy
  on top of a stable mixer).

### 7.5 Input manager

- Sources: board buttons (BSP), USB HID gamepads, USB HID keyboards (and mice), touch, analog
  (paddle). Merged into **logical devices**.
- **Gamepad**: standard layout (A/B/X/Y, L1/R1/L2/R2, L3/R3, Start/Select, D-pad, 2 sticks, analog
  triggers), up to 4 players. Mapping database from the reference auto-mapping (first-use mapping UI
  is a launcher feature), stored in `/sd/launcher/gamepads.db`.
- **Keyboard**: HID usage codes + modifiers + translated text events (keymap selectable).
- **Reserved system input**: a HOME/MENU chord (board-defined, e.g. MENU long-press or Select+Start)
  always goes to the launcher → overlay menu. Apps receive `PAPP_EV_PAUSE/RESUME/EXIT_REQUEST`, not
  the raw chord. (Fixes "touch apps quit when you touch the top strip".)
- Both polling (`papp_input_gamepad(0, &st)`) and an event queue.

### 7.6 Storage & paths

```
/sd/apps/<id>/<id>.papp   app bundle (+ icon.png, assets)      → papp_fs_app_dir()
/sd/data/<id>/             per-app writable data, saves, config → papp_fs_data_dir()
/sd/roms/...               shared content, opened via UI file picker or associations
/sd/launcher/              launcher config, gamepad DB, app index cache, crash logs
```

All file handles are tracked per app. `stat` returns real sizes (the `_fstat` bug was in app code;
now it's in one SDK place backed by a real `FS.stat`). FATFS LFN config stays mandatory.

### 7.7 Launcher application

- **UI**: LVGL v9 (already proven in a PAPP), resolution-independent: app grid/carousel, details page,
  settings (volume, brightness, gamepad mapping, keymap), file browser with associations, about/board info.
- **Index cache**: scan `/sd/apps` once, cache manifests + icons in `/sd/launcher/index.bin`; rescan on
  change (dir mtime) — fast boot with many apps.
- **File associations** from manifests: picking `game.nes` launches the app that declares `.nes`, with
  the path as `argv[1]`. This replaces `get_ota_slot()`.
- **Overlay** (in-app menu): resume, volume, brightness, screenshot, app-registered items
  (e.g. "Save state" / "Load state" via `SYS.register_menu_item`), quit.
- **Developer mode**: the DevLink service (§7.8).
- **Start at boot**: Settings → "At boot" picks an app with the search screen ("None" turns it off);
  the path is in `/sd/config/autostart.txt`. After boot a "Starting X" screen counts 2.5 s; a touch
  held 300 ms, a pad button or a key opens a box "Quit to launcher" / "Start X". Skipped after a crash
  notice or a panic/watchdog reset, so a crashing app can't lock the board in a loop; moving the app
  to another folder updates the file.
- **Self-update**: launcher A/B OTA from `/sd/update/launcher.bin` (the OTA partitions are no longer
  needed for emulators once they are PAPPs).

### 7.8 DevLink — remote development service

A launcher service that lets a developer (or an automated agent) drive the device **without touching
it**: put an app on the SD card, run it, watch its logs, see the screen, press buttons, read crash
reports, iterate. It replaces and extends the reference `PAPU`/`RUNR` serial protocol. It is always on
in development builds and can be switched off in release builds.

**Transport**: USB-Serial-JTAG on most boards, UART0 for the Elecrow AIO's CH340 bridge, or
authenticated TCP on the configured Wi-Fi interface (port 3234 by default). TCP uses a device
pairing PIN stored in NVS and printed to the local serial console at boot. Network DevLink keeps
Wi-Fi available while an app runs, so a host can continue to monitor or control it. Only enable it
on trusted networks; the existing serial transports remain available.

**Framing** (binary, CRC-protected, so logs and data never get mixed up):

```
 0xA5 'D' | type u8 | flags u8 | seq u16 | len u32 | payload[len] | crc32 u32 (over type..payload)
```

Requests carry a sequence number; the response echoes it. The device also sends unsolicited frames
(`LOG`, `EVENT`) with `seq = 0`. All `ESP_LOG` output and the app's log calls go out as `LOG` frames;
when no host is reading they are dropped (never block the device).

| Command | Purpose |
|---|---|
| `AUTH` (TCP only), `PING` / `INFO` | pair to the device; launcher version, ABI version, board id + caps, free memory, uptime, app state |
| `LS`, `STAT`, `MKDIR`, `RM`, `RENAME` | SD card file management |
| `PUT` (begin / data chunks / end + CRC32) | upload any file; written to a temp name and renamed only after the CRC matches |
| `GET` | download a file (saves, logs, screenshots, crash files) |
| `RUN path [args]`, `STOP`, `STATUS` | start an app, request exit / force-stop it, query state; `EVENT app_exit(code)` when it ends |
| `SHOT` | capture the composited screen (RGB565, optionally PNG) — lets the host *see* what the app draws |
| `INPUT` | inject gamepad buttons / key presses / touch points into the input manager |
| `CRASH` | last crash record (+ core dump from the `coredump` partition) |
| `REBOOT` | reboot; for launcher updates, upload `/sd/update/launcher.bin` with `PUT` and use the normal updater prompt |

**Host tool** `tools/papp_dev.py`: every command works over serial or TCP (`--host IP`): `info`, `ls`, `put`, `get`, `rm`, `run`, `stop`, `log`, `shot`,
`input`, `crash`, `reboot`, and **`deploy`** = build-output → `PUT` → `RUN` → stream logs until the app
exits or a timeout, returning the app's exit code as the process exit code. This makes the
edit → build → deploy → observe loop scriptable end to end, which is what allows autonomous work.

For example, read the PIN over a serial connection once, then use any command over Wi-Fi:

```sh
python tools/papp_dev.py --port /dev/ttyUSB0 pin
python tools/papp_dev.py --host 192.168.1.50 --token 123456 info
python tools/papp_dev.py --host 192.168.1.50 --token 123456 deploy build/apps/hello/hello.papp
```

Set `PAPP_DEVLINK_TOKEN` to avoid putting the PIN in shell history; when neither is supplied, the
tool prompts for it. To reset the PIN, erase the launcher's NVS partition. The TCP listener accepts
one client at a time and applies a one-minute lockout after five failed PIN attempts.

---

### 7.9 Pins, timers and interrupts (IRQ)

Apps get the board's free header pins and the P4's hardware timers, with interrupts on the core they pick
(`components/lx_loader/lx_svc_irq.c`, interface in `papp_abi.h`, test `apps/irq_test`).

**Which pins.** Each board says which GPIOs are free for apps: `bsp_app_gpio_mask()` (weak, 0 = none), asked
at every app start so a board can leave out pins an add-on it found uses (the Guition 4.3" GPIO gamepad).
Only pins on a header or port that nothing on the board uses; strapping pins (34-38: BOOT and the console)
and the USB-Serial-JTAG pins stay out. From the boards' schematics:

| Board | Free pins (header) |
|---|---|
| Guition 4.3" | 28-33, 49-52 (JP1); with the GPIO gamepad fitted only 31 |
| Guition 7" | 1-5, 32, 33, 45-47 ("Expand IO" JP1; its GPIO 20 is the amplifier enable) |
| M5Stack Tab5 | 2-7, 16-19, 45, 47, 48, 51, 52 (M5-Bus), 49 (Ext.Port1; G0/G1/G50 are the keyboard's), 53, 54 (Grove Port A) |
| Waveshare 3.5" | 2-5, 30-33, 46-48, 51, 52 (J6) |
| Elecrow kit | 45, 46 (I2C2, shared with the camera connector), 47, 48 (UART connector, level-shifted) |

**Interrupt per core.** The P4's GPIO block has four interrupt lines, each with its own per-pin enable bit and
status register; IDF's GPIO ISR service uses line 0 only (IDF-7995), e.g. for esp_hosted's SPI handshake. Apps
get line 1 on core 0 and line 2 on core 1, with dispatchers of ours, so app pins never meet IDF's. Timers are
gptimers (1 MHz). An interrupt is allocated on the core of the task that allocates it, so every allocation and
free runs on a short-lived task pinned to that core; that task runs only while its core is in no interrupt,
so a pin or timer removed there is never in the middle of its handler.

**Deferred and direct.** Deferred (default): the dispatcher queues an event (time, count, level, events
dropped before it) in the handle's ring and gives its semaphore; an app thread takes it with `wait()`
(`papp_irq_thread` makes one that calls a function per event). Direct: the app's function runs inside the
interrupt. Everything an interrupt touches is IRAM code and internal RAM, so it works while the caches are
off (flash writes; `code_alloc` maps memory): a direct handler is accepted only if it is in the app's FAST
region and that region is in SRAM, and it gets what it may call in a `papp_irq_ctx_t` (pin read/write, signal
another handle, re-arm a timer, all IRAM). The timers need `CONFIG_GPTIMER_ISR_CACHE_SAFE` and
`CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM` (launcher sdkconfig.defaults). Level interrupts disable themselves after
firing until `enable()`.

**Measured on the Tab5** (`irq_test`, the pin toggled by the app as an output sees its own edges; the
Guition 7" with a v1.x chip gives the same, the v3.2 7" ~3 µs on an idle core too):

| | Core busy | Core idle | `PAPP_IRQ_AWAKE` |
|---|---|---|---|
| pin write → direct handler | 3 µs (max ~10) | 20 µs (max ~70) | 3 µs (max ~40) |
| pin write → deferred event | 3 µs | 27 µs (max ~110) | |
| event → waiting thread | ~30 µs | ~130 µs | |
| 1 ms timer, interval at the interrupt | | 960-1045 µs | 984-1016 µs |
| 50 µs direct timer, 1 s, code_alloc every 2 ms | 19,765-19,812 of 20,000 (longest gap 100 µs) | | |

On v1.x chips an interrupt that finds its core in the idle task starts about 17 µs late (not on the v3.2
chip). It is not the sleep (WFI): an
idle hook that keeps the idle task spinning changes nothing, while any task above it removes it.
`PAPP_IRQ_AWAKE` therefore runs a priority-1 task on that core that spins while such an interrupt is
attached, giving the core up for a tick every 100 ms so the idle task still runs (task watchdog, freeing
deleted tasks); app threads (priority 3 and up) are not affected. Edges closer together than the latency merge
into one interrupt (GPIO status is one bit per pin).

**Teardown.** `lx_svc_irq_end_run` runs first in `lx_svc_end_run` (after the app's threads are gone, before
its memory is freed): every interrupt detached and freed on its core, the dispatchers freed, timers deleted,
configured pins reset to inputs without pulls. Checked with `irq_test leave` (left attached) and `hang`
(forced stop with two threads in `wait()`, a 50 µs direct timer and a toggling pin).

### 7.10 Bluetooth: Bluedroid, USB dongles, speaker, gamepads

The Bluedroid boards (Guition, Tab5, Elecrow; `CONFIG_BT_BLUEDROID_ENABLED` in `sdkconfig.board`) build the
dual-mode stack, because Bluetooth audio (A2DP) is Classic and NimBLE is LE only. It costs about 0.5 MB of
image; its memory comes from PSRAM first.

**One stack, two users** (`components/lx_loader/lx_bt.c`). Bluedroid has global state, so one core owns it:
`lx_bt_acquire(user, need_classic)` / `lx_bt_release(user)`, users = the app's BLE interface, the speaker and
the gamepads. Bluedroid takes one GAP BLE and one GATT client callback, so `lx_bt.c` registers its own and
fans every event out to slots (`lx_bt_ble_handlers`: app, pads, scan, test); each slot filters its own.
Start and stop run as jobs on the "lx_bt" task (internal stack: Bluedroid must not start on a PSRAM stack).
The stack stops when the last user lets go. Controller, chosen at start:

- a **USB Bluetooth dongle** if one is plugged in and has LE (Classic + LE: needed for the speaker);
- else the **C6** over esp_hosted VHCI (LE only; the C6 filter answers the two commands its controller
  lacks). `need_classic` then fails: the speaker says it needs a dongle.

**USB transport** (`lx_bt_usb.c`, a third client of the USB host after HID and XInput). Bluetooth USB class
E0/01/01: HCI commands as class control requests, events on the interrupt IN endpoint (reassembled from its
16-byte packets), ACL on the bulk pair; SCO is not used. Before Bluedroid starts, the transport resets the
dongle and reads its features. Bluedroid with BLE built in asserts on a dongle without LE (Bluetooth
1.x-3.0), so for one the transport answers LE itself: LE commands and Write LE Host Support complete with
zeros, the features say LE, page 1 is answered here. Such a dongle runs Classic only (speaker, Classic pads;
BLE pads never connect). An old one (genuine CSR, about Bluetooth 1.2: no SSP, legacy PIN pairing) refuses
Read Local Supported Commands and Read Local Extended Features with a Command Status, where Bluedroid waits
for a Command Complete and asserts on a failed one: refused controller / information / status commands are
answered as done (no supported commands, extended page 0 = its own features). The board takes the master
role on every link it accepts (Accept Connection Request, role 0): Bluedroid accepts the first incoming link
as slave, and a second one then put the board in two piconets. Counterfeit CSR8510 dongles (0A12:0001, HCI revision = LMP subversion 0x1d86) say
their last features page is 0 while setting the extended-features bit; Bluedroid reads LE host support from
page 1, so the transport reports page 1 and answers it itself (SSP, LE, LE + BR/EDR host bits). An unplugged
dongle gets every further command refused at once, so the stack shuts down without timeouts. Realtek dongles
need a firmware download, not done yet.

**Speaker** (`lx_bt_speaker.c`, Settings → Speaker on the home screen). Scan (general inquiry, ~13 s; audio
devices by class of device first), pick, pair (SSP "just works", legacy PIN 0000), A2DP source with
Bluedroid's SBC encoder. The chosen speaker is kept in `/sd/config/bt_speaker.txt` (`addr=`, `name=`, `on=`);
while it is on and a dongle is present the launcher keeps the stack and calls it after 10, 20, 40, then every
60 s. The board is connectable but not discoverable, so a speaker switched on later can reconnect by itself. The home
status bar shows the Bluetooth symbol and the speaker's name while it is connected, and the "System ready"
line says "connected" / "disconnected" for 5 s on a change. While audio
streams, `lx_audio_set_tap` hands the mixer's 48 kHz blocks to the speaker (linear resampling to the A2DP rate,
usually 44.1 kHz; at most 60 ms queued: the codec clock runs a bit fast, so a deeper ring only adds delay; master volume applied in software) and the codec plays silence, which
still paces the mixer. Tested: counterfeit CSR8510 A10 with an A2DP speaker on the Guition 7"; with a
DualShock 4 and the speaker together, an old genuine CSR (Bluetooth ~1.2, 8 ACL buffers): pad ~230 reports/s,
longest gap < 100 ms, sound steady (its basic-rate radio keeps the audio channel congested, as expected).

**Gamepads** (`lx_bt_pads.c`, same Settings screen: the scan lists controllers too). Classic pads (DualShock 4,
DualSense, Switch Pro) go through Bluedroid's HID host and call the board when switched on. BLE pads (Xbox
Wireless, 8BitDo BLE mode) use a small HID-over-GATT client: encrypt, read the report map, PnP ID and report
references, subscribe; reports (prefixed with the report id when the map has ids) go to `lx_input_hidpad`,
so a Bluetooth pad is a player like a USB one. Bluedroid has no background LE connections, so one low-duty
direct connection request (scan window 12.5 %) stays pending, rotating over the saved BLE pads, ~30 s each.
Saved in `/sd/config/bt_pads.txt` (`addr name`, BLE: `addr le<type> name`); bonds in Bluedroid's NVS store.

**Speaker and pads together.** Nothing is switched off while a device is connected. The one conflict: while
the CSR8510 pages an absent speaker it carries no LE data at all, so every BLE pad's reports stop for the whole
page (links stay up). With a pad connected a call therefore pages 2.56 s instead of 5.12 s, comes every 30,
60, then 120 s, and only once no pad input has changed for 20 s (`lx_input_hidpad_still_ms`: buttons, or a
stick / trigger moved past its noise): a missing speaker never freezes a game. A speaker switched on reconnects
by itself anyway (the board stays connectable). AFH: the Classic and LE channel maps leave out the Wi-Fi channel
±11 MHz (checked every 10 s, from `lx_net_channel`, which does not wait for the C6 the way `lx_net_info` does).

**BLE pad encryption.** A bonded pad asks for security when it connects and Bluedroid encrypts the link by
itself; asking as well gets the request queued, and when it runs on the already encrypted link it fails ("No
key available") and Bluedroid deletes the bond. So `lx_bt_pads.c` asks only when pairing, or for a saved pad
that started nothing within 5 s. Bluedroid's per-notification warnings (reports before the client registers
again, 60+ a second per pad) are turned off (`BT_APPL`, `BT_GATT` at error level): on the console they blocked
its task and stalled the pads' set-up. `CONFIG_BT_BLE_SMP_REMOVE_BOND_ON_PAIR_FAIL_AS_CENTRAL` is off (Bluedroid
deleted a pad's bond on any failed encryption, a timeout at start-up included): a saved pad's keys go after 3
failures in a row (`lx_bt_pads.c`), an app's outgoing link keeps the old rule (`lx_svc_ble_bd.c`).

**CSR8510 quirks** (`lx_bt_usb.c`). It reports no LE buffers (LE Read Buffer Size = 0): its 10 ACL buffers are
shared, and Bluedroid then gives all LE links together one transmit credit. The dongle sometimes holds an LE
packet for seconds, and with one credit every pad's requests (MTU, service search, pairing) waited. The
transport splits the buffers by adjusting the two replies: 4 for LE, 6 for Classic (given all 10 to
Classic, the speaker's stream stopped for good when a DualShock 4 connected, and the dongle reset with
hardware error 0x28). It does not run a speaker and a Classic pad together reliably: while the speaker
streams it hears no pad calling (PS button), however long its page scan; a speaker joining a connected pad
often dropped the pad's link (supervision timeout) or reset the dongle (0x28). Use another dongle for both.
Sniff mode stays off: the "sniff allowed" bit is cleared from the link policies Bluedroid writes. An LE link it sets up while
it pages comes up broken (encrypted, nothing sent gets through), so a pad's connection request is cancelled
before each speaker call and none is made while it pages. `bt status` shows `acl_unacked` (packets the dongle
has not completed, per link) to spot a stuck one.

**Test aids.** DevLink `bt` (`papp_dev.py bt status|scan --wait|results|pair ADDR|speaker ADDR [NAME]|speaker
on|off|forget-speaker|forget-pads`; `pair ADDR` of a saved pad pairs it again): `status` = JSON with each pad's
reports, longest gap, stalls (gaps >= 100 ms) and stalls while in use (within 2 s of an input change) since the
last read. A launcher built with `-DLX_BT_TESTPAD=1` (`lx_bt_testpad.c`) poses as a BLE gamepad on its C6
("LX Test Pad XXXX", report every 15 ms once the host has subscribed, as a real pad: esp_hidd notifies
before that, and a pad streaming during the host's service search slowed its answers past the 20 s set-up
limit; 50 s playing with X = counter, 40 s resting; after two failed
encryptions in a row it drops its keys, as a pad put in pairing mode would) for testing pads without one in hand.
A Windows PC cannot stand in for the speaker: the counterfeit CSR8510 reports hardware error 0x28 a few seconds
into any link with the PC's Intel adapter, and Bluedroid resets it.

### 7.11 Video, audio and JPEG: DISP v5 `blit_yuv`, CODC

The P4 has no video decoder: its H.264 block only encodes. So the decoders are software behind one interface
(CODC), which picks the implementation: hardware where a chip has it (JPEG now; a later chip's H.264 decoder
would slot in with no change to apps), else code built into the launcher, else a **codec module** from the SD
card. Espressif's `esp_h264` decoder is Baseline only (tinyH264, ~9 fps at 640x480), useless for ordinary MP4s,
so H.264 is FFmpeg's decoder: too big for the 3 MB launcher slot, hence a module.

- **`blit_yuv` (DISP v5, `lx_comp_blit_yuv`)**: a decoder's planar YUV 4:2:0 frame (I420 / FFmpeg's yuv420p:
  three planes with strides) is repacked by the CPU into the P4's own YUV420 layout and turned into RGB565 and
  scaled by the PPA's SRM straight into the surface's buffer. The P4 layout is packed, 12 bits a pixel, every
  line holding Y pairs after one chroma sample: U on even lines, V on odd ones (line 2k = `U Y Y U Y Y ..`,
  line 2k+1 = `V Y Y ..`); `codec_test` confirms it against a CPU model. BT.601 / BT.709, limited or full
  range. The scaled frame must lie inside the surface (no clipping), at even coordinates. Before the SRM the
  surface buffer is written back (`esp_cache_msync` C2M): the SRM only invalidates its output, which would drop
  the CPU's drawing in the lines it shares with the frame. Cost on the v3.2 7": 640x360 ~9 ms repack + ~6.5 ms
  SRM to 800x450; 1280x720 ~38 + ~17 ms. Both are bound by PSRAM (about 100 MB/s for a copy; the repack reads
  and writes ~1.5 bytes a pixel), so neither a word-wide loop nor running it from IRAM changed it. For video:
  make the surface the frame's size, blit at scale 1, and let present() scale in the background.
- **CODC v1 decoders (`lx_svc_codec.c`)**: `open(cfg)` (codec, cores 0/1/2, thread priority, the container's
  set-up data: an MP4's avcC or AudioSpecificConfig), `send(packet, pts)` (copied; `PAPP_E_LIMIT` = output is
  waiting, receive it and send the packet again; `NULL` = end of stream), `receive_frame` (planar 4:2:0 in the
  decoder's memory, valid until the next call, with pts, keyframe and `PAPP_YUV_*` flags for `blit_yuv`) /
  `receive_audio` (PCM, interleaved stereo; the first output of a packet carries its pts, the rest
  `PAPP_PTS_NONE`), `flush` (seek), `close`; `codec_info` says what is there without loading anything. Up to 8
  decoders per app, all closed when it ends. A decoder is used by one thread at a time.
  - Built in: **MP3 and AAC** (AAC-LC + HE-AAC; ADTS, or raw MP4 frames with the AudioSpecificConfig) on
    esp_audio_codec 2.5.0's simple decoder, which finds frames in any split of the data (Espressif Modified MIT
    licence; ~200 KB of the launcher image). 10 s of 44.1 kHz stereo: MP3 ~480 ms, AAC ~360 ms on one core
    (3.6-4.8 %). Built-in decoders run inside service calls.
  - **Codec modules** (`/sd/launcher/codecs/<codec>.papp`, e.g. `h264.papp`): PAPP v2 images built with
    `papp_build --module`; their entry `_papp_start(PAPP_MODULE_ARGC, argv)` (module_crt0.c: runs the
    constructors, no main) fills `argv[0]` with a `papp_codec_module_t` (magic `CMOD`, `max_cores`, name and the
    decoder functions). The Store installs them (launcher 0.37.1): the catalogue's `codecs` section lists them
    (`codec_<name>.papp` on the apps release), an app's `"needs": ["h264"]` (apps/store.json, or a community
    app.json) puts them on the card with the app, and reading the catalogue updates copies already there. The
    launcher loads one on an app's first `open` of its codec (`lx_module_load` in
    lx_app.c: the app loader's `load_image` into launcher memory, on a helper task with an internal stack because
    of `esp_mmu_map`) and unloads it when the app ends. Its imports resolve to the same interfaces as the app's, so
    its memory and threads are the app's (tracked, reclaimed at teardown: its decoders are not closed then, the
    module is just unmapped) and its code runs on the app's threads outside `SVC_ENTER`, like app code. It has a
    newlib of its own: `libc_glue.c` built with `-DPAPP_MODULE` keeps the module's `struct _reent` per thread in
    a table keyed by `thread_self` (the THRD TLS slot holds the app's). A crash inside a module is not decoded by
    `papp_dev.py crash` (the record knows the app's image only): the load line in the log gives its address.
  - **H.264 module** (`apps/codecs/h264`): FFmpeg 7.1.1's decoder (LGPL 2.1+; all profiles FFmpeg decodes as
    8-bit 4:2:0), Annex B in pieces of any size (FFmpeg's parser cuts access units) or MP4 access units with the
    avcC; cores = 2 is frame threading (pthreads on THRD: `pthread_shim.c`, 32 KB internal stacks). Every FFmpeg
    call runs on a thread of the decoder's own (the "runner", the caller waits on a semaphore): FFmpeg needs ~20 KB
    of stack in places (the Annex B parser, VLC tables at open) and an app's main thread has 16 KB. One FFmpeg file
    is patched (`patches/libavcodec/h264_slice.c`: a 34 KB `H264SliceContext` swap on the stack done in pieces);
    `aom_film_grain.c` is in the file list for an upstream h264-only build bug. ~1 MB image at -O2 (-O3: 1.5 MB, no
    faster), loads in ~0.35 s. 640x360 Main profile (Windows Media Foundation's encoder, CABAC): 36 fps on one
    core, 43-45 fps on two, v3.2 and v1.x alike; frame threading overlaps little for this clip (pinning the
    workers to separate cores changed nothing): profiling it is open.
- **CODC v1 JPEG**: `jpeg_info`, `jpeg_decode` (RGB565 into the app's buffer with a stride) and
  `jpeg_encode` (RGB565, 4:2:0, quality 1..100) on `esp_driver_jpeg`. The engine's output buffer must be
  cache-line aligned and sized and the decoder writes whole MCUs (16x16 for 4:2:0), so the launcher decodes and
  encodes through its own buffers (`jpeg_alloc_*_mem`, grown as needed) and copies. Engines are made on the first
  call and deleted, with the buffers, in `lx_svc_end_run`; one picture at a time (a mutex). The decoder refuses
  sizes that are not multiples of 8 (`PAPP_E_UNSUPPORTED`; decode those in software). 640x360: decode ~14 ms
  (72 fps), 1280x720 ~51 ms (software MJPEG in the media player: ~37 ms at 640x360).

## 8. App lifecycle & robustness

### 8.1 App context

```c
typedef struct app_ctx {
    const papp_manifest_t *mf;
    multi_heap_handle_t psram_heap, fast_heap;      /* arenas, destroyed wholesale  */
    res_list_t threads, mutexes, queues, timers, files, dirs, surfaces, streams, gfx_jobs;
    void *slow_base, *fast_base, *exec_map;
    volatile uint32_t state;                        /* RUNNING / EXITING / KILLED   */
    crash_ctx_t crash;                              /* region bases for symbolising */
} app_ctx_t;
```

Every ABI call that creates something registers it in the context; destroy calls unregister.

### 8.2 Exit paths

1. `app_main()` returns or `papp_exit(code)` → destructors → teardown.
2. User picks **Quit** in the overlay → `EXIT_REQUEST` event → grace period (e.g. 2 s) → **forced
   teardown**: suspend/delete every tracked thread, close streams/surfaces/files, destroy arenas,
   unmap, release PPA/DMA jobs. Safe because app threads can only be blocked on launcher-owned
   primitives — never inside a driver.
3. Teardown verifies `heap_caps_get_free_size()` returns to the pre-launch value (debug builds assert).

### 8.3 Crashes

A fault in app code goes through the ESP-IDF panic handler (unrecoverable). We make it
*diagnosable*: a panic hook writes `{app id, version, mepc, ra, mtval, sp, region bases}` to
`.noinit`/RTC memory; after the reboot the launcher shows *"Quake crashed at SLOW+0x1A2B4"* and writes
`/sd/launcher/crash/<id>-<n>.txt`; `papp-symbolize` maps it to `function:line` using the app ELF.
A task-watchdog on `papp_main` (optional per manifest) detects hangs.

---

## 9. SDK (`papp-sdk/`)

```
papp-sdk/
  include/papp/        papp.h, sys.h, mem.h, thread.h, fs.h, disp.h, gfx.h, img.h, audio.h,
                       input.h, kv.h, ui.h   (public API, versioned with the ABI)
  runtime/             crt0.c        entry → .init_array → main(argc, argv) → .fini_array
                       syscalls.c    newlib: _open/_read/_write/_lseek/_close/_fstat/_stat/
                                     _gettimeofday/_unlink/_rename/_isatty/_sbrk(fail)
                       malloc.c      malloc family → MEM (size policy: small→FAST, large→LARGE)
                       locks.c       newlib __retarget_lock_* → THRD mutexes
                       cxx.c         operator new/delete, __cxa_* minimal (no exceptions by default)
                       stubs.c       generated interface stubs + import table
  libs/sdl2-lite/      SDL2 subset (video surfaces, audio, events, timers) on top of the ABI —
                       shared by OpenTyrian, Duke3D, and future ports
  cmake/               papp.cmake (papp_add_app(...)), toolchain-riscv32-esp.cmake
  ld/papp.ld           SLOW/FAST output sections, .init_array, KEEP entry
  tools/papp-pack.py   ELF → .papp v2 (sections, relocs, imports, manifest, CRC, validation)
        papp-upload.py / papp-run.py / papp-log.py / papp-symbolize.py
  templates/           hello (C), sprite demo, lvgl, cpp, sdl2-lite game
  tests/conformance/   ABI conformance PAPP — exercises every interface, prints PASS/FAIL
```

Minimal app with the SDK:

```c
#include <papp/papp.h>

PAPP_MANIFEST(
    .id = "demo.sprite", .name = "Sprite Demo", .version = "1.0.0",
    .main_stack = 16 * 1024,
    .requires = PAPP_REQ(DISP, 1) | PAPP_REQ(INPT, 1), .optional = PAPP_OPT(AUDI, 1));

int main(int argc, char **argv)
{
    papp_surface_t *s = papp_disp_surface_create(&(papp_surface_cfg_t){
        .width = 400, .height = 240, .format = PAPP_PIXFMT_RGB565, .scale = PAPP_SCALE_FIT_ASPECT });
    while (!papp_exit_requested()) {
        papp_gamepad_t pad; papp_input_gamepad(0, &pad);
        uint16_t *fb = papp_disp_lock(s);
        draw(fb, &pad);
        papp_disp_present(s, PAPP_PRESENT_VSYNC);
    }
    return 0;   /* everything is reclaimed by the launcher anyway */
}
```

Build: `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PAPP_SDK/cmake/toolchain-riscv32-esp.cmake &&
cmake --build build` → `build/demo.sprite.papp`. Only the `riscv32-esp-elf` toolchain is needed — no
ESP-IDF project.

---

## 10. Compatibility with existing `.papp` v1 binaries

v1 apps are linked at absolute `0x4A000000` with no relocations. A **v1 compatibility adapter** is
possible only if the loader can map the exec region at exactly that virtual address. Recommendation:
**don't invest in binary v1 compatibility**; instead re-build the four ports against the SDK
(Phase 6 of the work plan) — the shim layers port mechanically and shrink considerably because the
libc glue moves into the SDK.

---

## 11. Key decisions (ADR candidates)

| # | Decision | Recommendation | Validate in |
|---|---|---|---|
| D1 | Relocation on host (custom format) vs on-device ELF loading | Host-side `papp-pack`; tiny loader | Spike A |
| D2 | Interface vtables + import table vs per-symbol dynamic linking | Interfaces (fast, versionable, clear errors) | design review |
| D3 | Executable internal SRAM (FAST region) mechanism | **Decided (Spike B):** option (a), `ESP_SYSTEM_PMP_IDRAM_SPLIT=n` + `MALLOC_CAP_EXEC` arena per app (cap `CONFIG_LX_APP_FAST_MAX_KB`, default 192 KB); FAST that doesn't fit goes to PSRAM. Cost: no W^X on internal RAM | Spike B — done |
| D4 | L2 cache 128 KB vs 256 KB | Measured (WORKPLAN, Spike B): 256 KB speeds PSRAM-heavy work like FAST does (128 KB/frame streaming case: 2.45 → 1.74 ms/frame) but costs 130 KB internal SRAM (490 → 360 KB free). **Decided: keep 128 KB** (more internal SRAM for FAST, stacks, DMA); revisit when a real emulator runs as a PAPP | Spike B — decided, revisit in Phase 7 |
| D5 | Forced kill of app threads | Allowed, because drivers are behind launcher queues | Spike C |
| D6 | libc in apps: toolchain newlib (static) vs launcher-exported libc | Static newlib from toolchain + SDK syscalls (as today, but shared) | Spike D |
| D7 | Fixed codec rate + mixer vs per-app I2S rate | Fixed rate + resampler | Phase 2 audio |
| D8 | Launcher UI toolkit | **Decided:** LVGL 9.2 for the home screen (launcher 0.9.0); app-facing UI screens (browser, menus) stay on the light `lx_gfx` renderer so they draw over apps cheaply | Phase 5 — done for the home screen |
| D9 | Emulators: PAPP vs keep OTA | Pilot then decide (gate in Phase 7) | Phase 7 |
| D10 | v1 binary compatibility | No — rebuild ports | — |

---

## 12. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| PSRAM-XIP + FAST region not fast enough for SNES/Genesis/NeoGeo | Emulators stay on OTA | Spike B early; keep OTA path as fallback "native firmware" app type |
| Executable SRAM requires weakening PMP W^X | Less memory protection | Only the reserved arena is RWX; rest unchanged |
| Cross-region relocation edge cases (relaxation, `JAL` across regions) | Load-time crashes | `--no-relax`; packer rejects unsupported relocs at build time; conformance tests |
| Forced kill while app holds a launcher-owned mutex | Launcher deadlock | Tracked mutexes are force-released/destroyed at teardown; launcher never shares mutexes with apps |
| Second board has a very different display (SPI, RGB, no DSI) | Compositor assumptions break | BSP `display_info` + compositor path without flip (copy) from day one; port board #2 before freezing ABI |
| ABI frozen too early | Painful v3 | Keep ABI "v2-preview" until second board + four ports + one emulator run on it |

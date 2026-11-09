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
| `MEM ` | Memory | `alloc(size, caps, align)`, `realloc`, `free`, `stats` | caps are **abstract**: `FAST` (internal), `DMA`, `LARGE` (PSRAM), `EXEC` (reserved, for future dynarec) |
| `THRD` | Threads & sync | thread create/join/exit (stack size + location, prio class, core), mutex, recursive mutex, semaphore, queue, event flags, timers | all tracked; v1 (launcher 0.5.0) has threads, mutexes, semaphores, queues; event flags + timers to come |
| `FS  ` | Files | open/read/write/seek/tell/close/stat/truncate, opendir/readdir, mkdir/remove/rename, `app_dir()`, `data_dir()` | sandboxed paths (§7.6) |
| `DISP` | Display / compositor | `info()`, surface create/destroy, `lock_back_buffer`, `present(async, vsync)`, palette for 8bpp, scale mode, dirty rects, direct-native mode | §7.3 |
| `GFX ` | 2D acceleration | PPA SRM (scale/rotate/mirror/convert), PPA blend (alpha, colour key), PPA fill, 2D-DMA copy, async + sync | generalises `sprite_blit` / `fb_copy` |
| `IMG ` | Image codecs | PNG decode, **HW JPEG decode** (P4 has a JPEG codec) to a chosen format | no colour-order hacks; returns real RGB565 |
| `AUDI` | Audio | stream open (rate, channels, format, latency), write/push, pull callback, pause, `get_queued` | launcher mixes + resamples |
| `INPT` | Input | gamepad state (player N, standard layout + analog axes + triggers), keyboard state, event queue (key/text/button/axis/touch/mouse/device-added), rumble (reserved) | §7.5 |
| `KV  ` | Settings | per-app key/value (int, string, blob); read-only global prefs (volume, brightness, language) | replaces NVS-path hacks |
| `UI  ` | Launcher dialogs | **file picker** (extension filter, start dir), message box, menu, volume, **Wi-Fi picker** (v4: the network chosen becomes the board's saved one; the app never sees the password), on-screen keyboard, progress, toast | rendered by the launcher on the overlay layer |
| `PWR ` | Power / board | battery %, charging, backlight, LED | optional |
| `NET ` | Network | Wi-Fi (up with the saved network, info, scan, join for the run, SoftAP, events), DNS, TCP/UDP sockets with poll, TLS client, HTTP/1.1 client | v1 (launcher 0.29); the first call keeps Wi-Fi on for the run; `docs/NET_BLE_PROPOSAL.md` |
| `BLE ` | Bluetooth LE | scan, connect, GATT client (discover, read, write, subscribe), GATT server (one table, values kept by the launcher, notify/indicate), advertising, events | v1 (launcher 0.29), Guition boards and Tab5; NimBLE runs only during the app's run; `docs/NET_BLE_PROPOSAL.md` |
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
- **Self-update**: launcher A/B OTA from `/sd/update/launcher.bin` (the OTA partitions are no longer
  needed for emulators once they are PAPPs).

### 7.8 DevLink — remote development service

A launcher service that lets a developer (or an automated agent) drive the device **without touching
it**: put an app on the SD card, run it, watch its logs, see the screen, press buttons, read crash
reports, iterate. It replaces and extends the reference `PAPU`/`RUNR` serial protocol. It is always on
in development builds and can be switched off in release builds.

**Transport**: USB-Serial-JTAG (the same cable used for flashing; no extra hardware). Later the same
protocol over TCP (Wi-Fi) — the framing is transport-independent.

**Framing** (binary, CRC-protected, so logs and data never get mixed up):

```
 0xA5 'D' | type u8 | flags u8 | seq u16 | len u32 | payload[len] | crc32 u32 (over type..payload)
```

Requests carry a sequence number; the response echoes it. The device also sends unsolicited frames
(`LOG`, `EVENT`) with `seq = 0`. All `ESP_LOG` output and the app's log calls go out as `LOG` frames;
when no host is reading they are dropped (never block the device).

| Command | Purpose |
|---|---|
| `PING` / `INFO` | launcher version, ABI version, board id + caps, free memory, uptime, app state |
| `LS`, `STAT`, `MKDIR`, `RM`, `RENAME` | SD card file management |
| `PUT` (begin / data chunks / end + CRC32) | upload any file; written to a temp name and renamed only after the CRC matches |
| `GET` | download a file (saves, logs, screenshots, crash files) |
| `RUN path [args]`, `STOP`, `STATUS` | start an app, request exit / force-stop it, query state; `EVENT app_exit(code)` when it ends |
| `SHOT` | capture the composited screen (RGB565, optionally PNG) — lets the host *see* what the app draws |
| `INPUT` | inject gamepad buttons / key presses / touch points into the input manager |
| `CRASH` | last crash record (+ core dump from the `coredump` partition) |
| `REBOOT`, `OTA` | reboot (normal / into download mode); upload a new launcher image (A/B) |

**Host tool** `tools/papp_dev.py`: `info`, `ls`, `put`, `get`, `rm`, `run`, `stop`, `log`, `shot`,
`input`, `crash`, `reboot`, and **`deploy`** = build-output → `PUT` → `RUN` → stream logs until the app
exits or a timeout, returning the app's exit code as the process exit code. This makes the
edit → build → deploy → observe loop scriptable end to end, which is what allows autonomous work.

---

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

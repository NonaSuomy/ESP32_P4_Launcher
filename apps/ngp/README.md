# Neo Geo Pocket

Neo Geo Pocket and Neo Geo Pocket Color emulator built as a native PAPP2 app.
It uses P4 Launcher APIs for the ROM picker, display, controls, audio, and SD
card access; it does not run through the PAPP1 compatibility layer.

## Build

From the repository root:

```sh
python apps/ngp/build.py
```

This writes `build/apps/ngp/ngp.papp`.

## ROMs

Place `.ngp` and `.ngc` files under `/sd/roms/ngp/`. The app opens P4 Launcher's
file picker when started. Game ROMs are not included. The emulator uses its
built-in BIOS emulation and does not require a separate BIOS file.

## Controls

- D-pad: directional input
- A/B: game buttons
- Start or Select: Option button
- X: pause/menu; choose **Exit Game** to return to P4 Launcher
- Y: launcher volume control

## Source and license

The emulator core is the Race Neo Geo Pocket port included in this directory;
see `core/license.txt` for its GPL-2.0-or-later terms and upstream notices.
The PAPP2 frontend uses the P4 Launcher PAPP SDK.

# AZIP Z-machine interpreter

AZIP runs Infocom and Inform interactive fiction in Z-code format as a native
PAPP2 app. It uses the P4 Launcher file picker, display, USB keyboard/gamepad,
and SD card APIs.

## Build

```sh
python apps/azip/build.py
```

This writes `build/apps/azip/azip.papp`.

## Stories and saves

Place `.dat` or `.z1` through `.z8` story files under `/sd/roms/azip/`. AZIP
opens a story picker when launched. Save files are written beside the story as
`<story>.sav`; the interpreter's undo feature is not supported.

## Controls

- USB keyboard: type commands; Enter submits; Backspace deletes
- Gamepad A: Space
- Gamepad B: Enter
- D-pad Up/Down: interpreter cursor keys
- Menu or L3: return to P4 Launcher

## Upstream and licensing

The interpreter core is Jzip V2.1 by John D. Holder, adapted for AZIP on
ESP32. See `LICENSE-JZIP` for its redistribution terms. The PAPP2 frontend is
adapted from the PAX AZIP port in the RetroESP32-P4 project.

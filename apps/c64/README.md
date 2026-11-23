# Commodore 64

Frodo 4.2 rebuilt as a native PAPP2 application for P4 Launcher. The app uses
the launcher's file picker, display surface, gamepad and keyboard input, audio,
and filesystem APIs. It accepts D64 disk and T64 tape images and can start the
BASIC environment when the picker is cancelled.

## Build

From the repository root, run:

```sh
python apps/c64/build.py
```

This creates `build/apps/c64/c64.papp` and copies the app icon alongside it.

## Files

Place C64 firmware ROMs in `/sd/roms/c64/bios/`:

| File | Size |
| --- | ---: |
| `basic.rom` | 8 KiB |
| `kernal.rom` | 8 KiB |
| `char.rom` | 4 KiB |
| `1541.rom` | 16 KiB |

The app then offers `.d64` and `.t64` files under `/sd/roms/c64`. ROMs and game
images are not included.

## Controls

- D-pad: joystick directions for ports 1 and 2, plus keyboard navigation
- A/B: joystick fire and Space
- Start: Run/Stop
- Select: F7
- L1: game menu; choose **Exit Game** to return to P4 Launcher
- Game menu also provides the existing emulator settings and button mapping

The Frodo core is GPL-2.0; see [`core/COPYING`](core/COPYING). The PAX-to-PAPP2
host adapter and its P4 Launcher bridge are included in this app's source.

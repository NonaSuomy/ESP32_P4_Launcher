# Game & Watch

This app runs the LCD-Game-Emulator core as a native PAPP2 program. It uses
P4 Launcher’s file picker, RGB565 display/PPA scaler, gamepad, audio, and
filesystem APIs. It does not load a PAPP1 image or depend on the PAX runtime.

## Build

From the repository root:

```sh
python apps/gw/build.py
```

This creates `build/apps/gw/gw.papp` and copies the app icon alongside it.

## Game images

Place compatible `.gw` or `.lcd` game images in `/sd/roms/gw/`. The app opens
the Game & Watch picker on launch. Game ROMs are not included.

## Controls

- D-pad: Game & Watch directional controls
- A/B: primary game controls
- Start: Game button
- Select: Time button
- X: in-game menu; choose **Exit Game** to return to P4 Launcher
- Y: launcher volume control

## Source and license

The emulator core is from [LCD-Game-Emulator](https://github.com/bzhxx/LCD-Game-Emulator),
snapshot `d3ccf35`. Its GPL-3.0 license is included in `core/LICENSE`. The
native PAPP2 frontend is based on the PAX Game & Watch port in the RetroESP32-P4
workspace and uses the P4 Launcher PAPP SDK.

# WonderSwan

WonderSwan and WonderSwan Color emulator built as a native PAPP2 app using
P4 Launcher APIs for ROM selection, display, input, and audio.

## Build

From the repository root:

```sh
python apps/ws/build.py
```

This writes `build/apps/ws/ws.papp`.

## ROMs

Place `.ws` or `.wsc` ROMs under `/sd/roms/ws/`. The app opens the launcher's
file picker when started. ROMs are not included.

## Controls

- D-pad: WonderSwan directional controls
- A / B: game buttons
- Start / Select: console buttons
- X: pause menu; choose **EXIT GAME** to return to P4 Launcher
- Y: launcher volume control
- L3: return to P4 Launcher

## Source

The emulator core is Oswan. Its upstream project is
[FranciscoPC/Oswan](https://github.com/FranciscoPC/Oswan).

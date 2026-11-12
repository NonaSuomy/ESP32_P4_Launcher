# SD card starter

First flash the launcher on the board, from your browser: see "Getting started" in the main README.

Copy the contents of this folder to the root of an empty FAT32 or exFAT card (exFAT needs launcher 0.29.4 or later), put it in the board,
then on Wi-Fi open the **Store** (first tile on the home screen) and choose **Install all** to download the apps.

```
apps/                    the apps (the Store puts them here)
apps/Education/          apps for students, in the Education folder of the home screen (only here, not in the Store)
config/gamepads.txt      controller button mappings shared by everyone (see below)
roms/<system>/           your ROMs, one folder per emulator
roms/doom/doom1.wad      Doom shareware episode
roms/quake/id1/pak0.pak  Quake shareware episode
roms/neogeo/gngeo_data.zip  data of the gngeo emulator (you still need your own neogeo.zip BIOS)
roms/duke3d/             your own duke3d.grp
roms/gba/                Game Boy Advance ROMs (.gba); optional gba_bios.bin (see below)
saves/doom/              Doom saves
Tyrian/data/             Tyrian 2.1, released as freeware, for OpenTyrian
```

No ROMs, BIOS files or commercial game data are here; add your own legally obtained copies.
This folder's `.gitignore` lists what may be shared, so what you add stays on your PC.

## Game Boy Advance

The **Game Boy Advance** emulator (gpSP, translating the game's code to RISC-V as it runs) plays `.gba` files from
`roms/gba/` at full game speed. Heavy games (Mario Kart, Castlevania) skip drawing some frames to keep the game and the
sound at full speed. It needs launcher 0.29.1 or newer for that translator and for drawing in the background; on older
launchers it interprets the code, slower. Without a BIOS it uses a built-in replacement, which runs most games; a few need the real
one: put your own dump as `roms/gba/gba_bios.bin`. Big ROMs (32 MB) load in pieces as you play, so the first seconds may
stutter. In game, X or the left screen edge opens the menu (save, load, restart, exit), Y or the right edge the volume.
Battery saves (`.srm`) and saved games (`.sav`) go to `odroid/data/gba/`.

## Controller mappings

Most USB pads work without setup. If the buttons of yours come out wrong, map it once in
**Settings → Map controller**. The launcher saves it in `/sd/config/gamepads_mine.txt`, one line
per controller model:

```
054C:0268  A=B B=A  # DualShock 3
```

`VID:PID`, then `<button the pad reports>=<button it should be>` for each button that differs,
then the pad's name.

To share yours: add its line to `config/gamepads.txt` here and open a pull request. Settings →
Apps downloads the newest `gamepads.txt` to every card, so the next owner of that pad doesn't
have to map it. Your own file always wins over the shared one.

## Wi-Fi network

The network you join in **Settings → Wi-Fi** is saved in `/sd/config/wifi.txt`, so it moves
with the card. To set up a board without its screen, write the file yourself:

```
ssid=MyNetwork
password=MyPassword
```

Delete the file, or choose Forget, to forget the network. The password is in plain text: keep
the file to yourself (git ignores it here).

## Education apps

`apps/Education/` holds apps for teaching, each in its own folder (`<app>/<app>.papp` + `icon.png`). Copy the
folder to the card as it is (or the whole starter card) and they appear in the **Education** folder on the home
screen. They are not in the Store. Their sources are in `apps/education/` of the project, and
`python apps/education/build.py` rebuilds them into this folder.

- **BLE Remote**: the board as a Bluetooth LE peripheral named `gution-p4`. A phone (any BLE app, e.g. nRF Connect)
  or a PC sets its mode, level, LED and message, and talks to its console over the Nordic UART service
  (`help` lists the commands). The screen shows the values and a log of what goes in and out.
- **Wi-Fi Remote**: the board as a web server. Open the address it shows (`http://<board>/`) in a browser on the
  same network to set the same values. Scripts can use `GET /state`, `POST /state` with JSON such as
  `{"level": 80, "led": true}`, or `http://<board>/set?level=80&led=on&msg=Hi` from the address bar.
  The **Network** button lists the networks nearby: the one you join (with its password, typed on the screen) becomes
  the board's saved network, as in Settings → Wi-Fi.
  Without a saved network (or with the Hotspot button) the board opens its own Wi-Fi network
  `P4-Remote-xxxx`, password `p4remote`, and the page is at `http://192.168.4.1/`.
- **Sprite Demo**: fly a spaceship sprite around space with the pad (D-pad or stick). The P4's picture engine (PPA)
  draws it. The **Sprite** buttons (or A) pick how: **Key on**, the ship is painted on magenta, the key colour, and
  the magenta is left out; **Key off**, the whole magenta rectangle is drawn; **Alpha**, every pixel has its own
  transparency, so the edges are soft and the ship glows. The ship turns to face where it flies, and L1 / R1 make
  it smaller or bigger. The zoom box shows the pixels 3x bigger. X exits.
- **LVGL Demo**: a screen built with LVGL, the free graphics library the launcher's own home screen uses: a slider
  that drives a gauge, a switch and an LED, a button that counts, a colour list and a light/dark theme; a live chart;
  a text box with an on-screen keyboard. Touch it, or use the pad: Up / Down pick a control, Left / Right change it,
  A presses, B goes back, L1 / R1 change tab, X exits. A USB keyboard types into the text box.
- **Game of Life**: cells live or die by two simple rules, and patterns walk, blink and grow. Touch the grid to draw
  cells (or rub them out), then Play, Step one generation, or pick a speed. Ready-made patterns: glider, blinker,
  and the glider gun that fires gliders for ever. Pad: A play / pause, B step, Y random, L1 / R1 speed, X exits.
- **Mandelbrot Race**: the same fractal drawn four ways, each one timed: on 1 core, on both cores of the P4 (about
  twice as fast), with the inner loop in fast internal memory (little change: the loop already fits in the cache),
  and in fixed point (whole numbers) instead of float. Tap the picture to zoom in 2x; deep in, float runs out of
  digits and the picture turns blocky while fixed point stays sharp. Pad: A race all, R1 zoom in, B / L1 zoom out,
  Left / Right pick a way, Y runs it, X exits.
- **Synth**: a two-octave keyboard (several fingers make a chord) with five wave shapes (sine, square, saw,
  triangle, noise), an octave switch and an echo. The scope shows the wave going to the speaker and the spectrum
  the frequencies in it: a sine is one bar, a saw a whole row of harmonics. Pad: A / B / Y play C / E / G, L1 / R1
  change the shape, Up / Down the octave, X exits. A USB keyboard plays too (A S D F G H J K and W E T Y U).
- **Touch Paint**: draw with several fingers at once; 12 colours, three brush sizes, Fill (flood fill), Eraser,
  Undo (twice = redo), Clear. **Save** writes a BMP to `/sd/pictures/` (it opens on a PC too); **Open** loads one
  back (24 or 32-bit BMP). Pad: A / B colour, L1 / R1 brush size, Y undo, Start save, Select open, X exits.
- **Physics Playground**: balls that fall, bounce and knock into each other; big ones are heavier. Touch an empty
  spot to drop a ball, drag one and let go to throw it. Sliders for gravity and bounciness; the energy bar shows
  that at 100% bounce the energy stays, and less bounce lets the balls come to rest. The PPA draws the balls.
  Pad: A add a ball, B shake, Left / Right (or the stick) tilt the box, L1 / R1 gravity, X exits.
- **Sensor Dashboard**: the board's radios and memory as sensors: Wi-Fi signal (dBm), Wi-Fi networks and Bluetooth
  devices nearby, free memory. Touch a tile to graph it. **Record** writes a line per sample to a CSV file in
  `/sd/logs/` (`dash_001.csv`, ...) to open in a spreadsheet. Uses the saved Wi-Fi network for the signal.
  Pad: Left / Right pick the value, A record, X exits.

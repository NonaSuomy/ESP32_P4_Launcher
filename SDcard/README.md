# SD card starter

First flash the launcher on the board, from your browser: see "Getting started" in the main README.

Copy the contents of this folder to the root of an empty FAT32 card, put it in the board,
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
saves/doom/              Doom saves
Tyrian/data/             Tyrian 2.1, released as freeware, for OpenTyrian
```

No ROMs, BIOS files or commercial game data are here; add your own legally obtained copies.
This folder's `.gitignore` lists what may be shared, so what you add stays on your PC.

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

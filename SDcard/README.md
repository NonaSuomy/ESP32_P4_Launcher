# SD card starter

First flash the launcher on the board, from your browser: see "Getting started" in the main README.

Copy the contents of this folder to the root of an empty FAT32 card, put it in the board,
then on Wi-Fi open the **Store** (first tile on the home screen) and choose **Install all** to download the apps.

```
apps/                    the apps (the Store puts them here)
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

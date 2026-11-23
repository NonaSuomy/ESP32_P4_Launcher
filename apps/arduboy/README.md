# Arduboy

Native PAPP2 emulator for Arduboy and Arduboy FX games, using the Ardens
ATmega32U4 simulator. Choose a `.hex` or `.arduboy` file from the app's ROM
picker. ROMs are not included.

## Build

From the repository root, run:

```sh
python apps/arduboy/build.py
```

The build writes `build/apps/arduboy/arduboy.papp`.

## ROMs and controls

Place game files under `/sd/roms/arduboy/`. The D-pad maps to Arduboy
directions, A/B map to the game buttons, X opens the pause menu, and L3 exits
to P4 Launcher.

EEPROM and FX save data are not persisted between launches yet.

## Source and licenses

The simulator is Ardens revision `e811cc6be72c8a802bd3265a84daf4165c177b93`
from [tiberiusbrown/Ardens](https://github.com/tiberiusbrown/Ardens). Its MIT
license and the licenses for bundled dependencies are included alongside the
sources under `vendor/Ardens/`.

# ScummVM AGI and SCI apps

This directory builds two independent native PAPP2 applications:

- `scummvm_agi` enables the AGI engine for games such as King's Quest and Space Quest.
- `scummvm_sci` enables SCI, including SCI32 games such as Quest for Glory IV.

They share the ScummVM front end and P4 Launcher PAPP2 platform adapter. The IDs
are `emu.scummvm_agi` and `emu.scummvm_sci`, which place them in the launcher's
Emulators category. Each app has its own engine icon with the ScummVM wordmark.

## Build inputs

The full ScummVM source and its configured static libraries are build inputs and
are intentionally not vendored here. Use a ScummVM checkout configured and built
for the same RISC-V target and toolchain as P4 Launcher, with AGI and SCI enabled
(and SCI32 enabled for SCI). It must contain the generated `config.h`, engine
archives, `plugins_table.h`, and base/common/backend/audio/gui libraries. The
ScummVM dependency library directory must contain `libfreetype.a`, `libjpeg.a`,
`libogg.a`, `libpng.a`, `libvorbis.a`, `libvorbisenc.a`, `libvorbisfile.a`, and
`libz.a` built for that target.

With those inputs available, build both apps with `python apps/build_all.py scummvm`, or build one engine directly:

```sh
python apps/scummvm/tools/build_scummvm_papp2.py agi \
  --scummvm-root /path/to/configured/scummvm \
  --deps-lib-dir /path/to/scummvm-dependencies/lib
python apps/scummvm/tools/build_scummvm_papp2.py sci \
  --scummvm-root /path/to/configured/scummvm \
  --deps-lib-dir /path/to/scummvm-dependencies/lib
```

The built artifacts are written to `build/apps/scummvm_agi/scummvm_agi.papp` and
`build/apps/scummvm_sci/scummvm_sci.papp`; icons are copied alongside them.
`SCUMMVM_ROOT` and `SCUMMVM_DEPS_LIB` can be used instead of the command-line
paths. These packages contain the engine binaries, not game data. Users provide
their own game files on the SD card.

The ScummVM engine code is licensed under GPL-3.0-or-later; see the upstream
ScummVM `COPYING` and `COPYRIGHT` files. Icon artwork attribution and license
are documented in `icons/ATTRIBUTION.md` and `icons/LICENSE`.

The app adapter and engine icons were tested against ScummVM source commit `10a220e2ec50f066c07d9be211a0f85934acce86`.

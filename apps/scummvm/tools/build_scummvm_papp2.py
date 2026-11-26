#!/usr/bin/env python3
"""Build ScummVM AGI or SCI as a native PAPP2 app for P4 Launcher."""
from __future__ import annotations

import argparse
import importlib.util
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "apps/scummvm"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", choices=("agi", "sci"))
    parser.add_argument(
        "--scummvm-root", type=Path,
        default=Path(os.environ.get("SCUMMVM_ROOT", ROOT / "third_party/scummvm")),
        help="configured ScummVM source/build tree (or SCUMMVM_ROOT)",
    )
    parser.add_argument(
        "--deps-lib-dir", type=Path,
        default=Path(os.environ.get("SCUMMVM_DEPS_LIB", ROOT / "third_party/scummvm-deps/lib")),
        help="directory containing the configured ScummVM third-party static libraries",
    )
    parser.add_argument("--out-dir", type=Path, help="output directory; defaults to build/apps/<app>")
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()

    scummvm = args.scummvm_root.resolve()
    deps_lib = args.deps_lib_dir.resolve()
    sdk_tools = ROOT / "papp-sdk/tools"
    spec = importlib.util.spec_from_file_location("papp_build", sdk_tools / "papp_build.py")
    if spec is None or spec.loader is None:
        parser.error(f"PAPP2 SDK build tool is missing: {sdk_tools / 'papp_build.py'}")
    papp_build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(papp_build)

    engine = args.engine
    required = [
        scummvm / "config.h", scummvm / "engines/plugins_table.h",
        scummvm / "engines" / engine / ("lib" + engine + ".a"),
        scummvm / "engines" / engine / "detection.o",
        scummvm / "audio/softsynth/mt32/libmt32.a",
        scummvm / "base/libbase.a", scummvm / "common/libcommon.a",
        scummvm / "common/compression/libcompression.a",
        scummvm / "common/formats/libformats.a", scummvm / "engines/libengines.a",
        scummvm / "audio/libaudio.a", scummvm / "backends/libbackends.a",
        scummvm / "graphics/libgraphics.a", scummvm / "gui/libgui.a",
        scummvm / "image/libimage.a", scummvm / "math/libmath.a",
    ]
    if engine == "agi":
        required.append(scummvm / "engines/agi/wagparser.o")
    else:
        required.append(scummvm / "video/libvideo.a")
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        parser.error("ScummVM engine build prerequisites are missing:\n  " + "\n  ".join(missing))

    libraries = [
        deps_lib / name for name in (
            "libfreetype.a", "libjpeg.a", "libogg.a", "libpng.a",
            "libvorbis.a", "libvorbisenc.a", "libvorbisfile.a", "libz.a",
        )
    ]
    missing = [str(path) for path in libraries if not path.is_file()]
    if missing:
        parser.error("ScummVM dependency libraries are missing:\n  " + "\n  ".join(missing))

    app_name = "scummvm_" + engine
    title = "ScummVM " + engine.upper()
    app_id = "emu." + app_name
    engine_objects = [scummvm / "engines" / engine / filename for filename in (
        ("detection.o", "wagparser.o") if engine == "agi" else ("detection.o",)
    )]
    sources = [
        APP / "scummvm_pax.cpp", APP / "scummvm_papp2_api.cpp",
        APP / "scummvm_plugins_papp2.cpp", APP / "pax_pthread_stubs.cpp",
        scummvm / "base/main.cpp", scummvm / "backends/fs/posix/posix-fs.cpp",
        scummvm / "backends/fs/posix/posix-iostream.cpp",
    ]
    includes = [
        APP, ROOT / "papp-sdk/include", ROOT / "components/papp_abi/include",
        scummvm, scummvm / "backends", scummvm / "backends/fs",
        scummvm / "backends/fs/posix", scummvm / "backends/fs/posix-drives",
        scummvm / "backends/saves/posix", scummvm / "backends/plugins",
        scummvm / "common", scummvm / "engines", scummvm / "base",
    ]
    flags = [
        "-DSCUMMVM_PAPP2_NATIVE", f"-DENABLE_{engine.upper()}=1",
        f'-DSCUMMVM_PAPP2_ID="{app_id}"', f'-DSCUMMVM_PAPP2_NAME="{title}"',
        "-DHAVE_CONFIG_H", "-DPAX", "-DPOSIX", "-DUSE_NULL_DRIVER",
        "-DUSE_TINYGL", "-DUSE_ELF_LOADER=0",
    ]
    if engine == "sci":
        flags.append("-DENABLE_SCI32=1")
    ldflags = [
        "-Wl,--wrap=fopen", "-Wl,--wrap=fclose", "-Wl,--wrap=fread",
        "-Wl,--wrap=fwrite", "-Wl,--wrap=fseek", "-Wl,--wrap=ftell",
        "-Wl,--wrap=getc", "-Wl,--wrap=fgetc", "-Wl,--wrap=fgets",
        "-Wl,--wrap=feof", "-Wl,--wrap=ferror", "-Wl,--wrap=clearerr",
        "-Wl,--wrap=fflush", "-Wl,--wrap=exit", "-Wl,--wrap=_free_r",
        "-Wl,--defsym=__preinit_array_start=0", "-Wl,--defsym=__preinit_array_end=0",
    ]
    extra_libs = [str(path) for path in required if path.suffix == ".a"]
    extra_libs += [str(path) for path in libraries]
    extra_libs += [str(path) for path in engine_objects]
    extra_libs += ["-lstdc++", "-lsupc++"]
    _, papp = papp_build.build(
        app_name, [str(path) for path in sources], includes=[str(path) for path in includes],
        libc=True, libc_glue=False, out_dir=str(args.out_dir or ROOT / "build/apps" / app_name),
        app_cflags=flags, extra_ldflags=ldflags, extra_libs=extra_libs,
        icon_dir=str(APP / "icons" / engine), jobs=args.jobs, version_dir=str(APP),
    )
    if not papp:
        parser.error("PAPP2 SDK did not package the app")
    print(f"Built native PAPP2 {title}: {papp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

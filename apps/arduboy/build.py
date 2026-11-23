#!/usr/bin/env python3
"""Build the Ardens Arduboy emulator as a native PAPP2 app."""

from __future__ import annotations

import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VENDOR = HERE / "vendor/Ardens"
SDK = ROOT / "papp-sdk"

SOURCES = [
    HERE / "native/papp2_main.cpp",
    *sorted((VENDOR / "src").glob("absim_*.cpp")),
    *sorted((VENDOR / "src/boot").glob("*.c")),
    HERE / "pthread_stubs.c",
    VENDOR / "deps/yyjson/yyjson.c",
    VENDOR / "deps/miniz/miniz.c",
    VENDOR / "deps/miniz/miniz_tdef.c",
    VENDOR / "deps/miniz/miniz_tinfl.c",
    VENDOR / "deps/miniz/miniz_zip.c",
]

INCLUDES = [
    HERE / "native",
    VENDOR / "src",
    VENDOR / "deps",
    VENDOR / "deps/bitsery/include",
    VENDOR / "deps/yyjson",
    VENDOR / "deps/miniz",
]

CXX_FLAGS = [
    "-std=gnu++17",
    "-D_Static_assert=static_assert",
    "-DARDENS_PLAYER=1",
    "-DARDENS_NO_DEBUGGER=1",
    "-DARDENS_NO_GUI=1",
    "-DARDENS_NO_SAVED_SETTINGS=1",
    "-DARDENS_NO_SCALING=1",
    "-DARDENS_VERSION_MAJOR=0",
    "-DARDENS_VERSION_MINOR=0",
    "-DARDENS_VERSION_PATCH=0",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-threadsafe-statics",
    "-fno-use-cxa-atexit",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-Wno-write-strings",
    "-Wno-narrowing",
    "-Wno-overloaded-virtual",
]


def main() -> int:
    if not (SDK / "tools/papp_build.py").is_file():
        raise SystemExit(f"PAPP SDK not found at {SDK}")

    sys.path.insert(0, str(SDK / "tools"))
    import papp_build  # noqa: E402

    per_file_cflags = {
        str(source): CXX_FLAGS for source in SOURCES if source.suffix == ".cpp"
    }
    # yyjson's generated parser has very long compile times at -Os on the P4 toolchain.
    per_file_cflags[str(VENDOR / "deps/yyjson/yyjson.c")] = ["-O0", "-g0"]

    papp_build.build(
        "arduboy",
        [str(source) for source in SOURCES],
        includes=[str(path) for path in INCLUDES],
        libc=True,
        out_dir=str(ROOT / "build/apps/arduboy"),
        app_cflags=[
            "-O2",
            "-DPAPP_APP_SIDE=1",
            "-DMINIZ_NO_STDIO=1",
            "-DMINIZ_NO_TIME=1",
            "-Wno-unused-variable",
            "-Wno-unused-function",
            "-Wno-sign-compare",
            "-Wno-maybe-uninitialized",
        ],
        extra_cflags=["-g0"],
        per_file_cflags=per_file_cflags,
        extra_libs=["-lstdc++"],
        icon_dir=str(HERE),
        version_dir=str(HERE / "native"),
        jobs=os.cpu_count() or 4,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

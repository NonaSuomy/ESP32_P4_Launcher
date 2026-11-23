#!/usr/bin/env python3
"""Build the Frodo Commodore 64 emulator as a native PAPP2 app.

Run from any directory with ``python apps/c64/build.py``. The result is
``build/apps/c64/c64.papp``.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SDK = ROOT / "papp-sdk"
CORE = HERE / "core"

sys.path.insert(0, str(SDK / "tools"))
import papp_build  # noqa: E402

SOURCES = [
    HERE / "native/papp2_main.cpp",
    HERE / "pax_main.cpp",
    CORE / "1541d64.cpp",
    CORE / "1541fs.cpp",
    CORE / "1541job.cpp",
    CORE / "1541t64.cpp",
    CORE / "C64_PC.cpp",
    CORE / "CPU1541_PC.cpp",
    CORE / "CPUC64_PC.cpp",
    CORE / "CIA.cpp",
    CORE / "Display.cpp",
    CORE / "IEC.cpp",
    CORE / "Prefs.cpp",
    CORE / "REU.cpp",
    CORE / "SID.cpp",
    CORE / "VIC.cpp",
    CORE / "VIC_table.cpp",
    CORE / "UserPort_4Player.cpp",
]

DEFINES = [
    "-DPAPP_APP_SIDE=1",
    "-DPAX_APP_SIDE=1",
    "-DPAPP_NATIVE_PAPP2=1",
    "-DIRAM_ATTR=",
    "-O2",
    "-fno-common",
    "-Wall",
    "-Wno-format",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-Wno-maybe-uninitialized",
    "-Wno-sign-compare",
    "-Wno-missing-braces",
    "-include",
    "stdint.h",
    "-include",
    "stdbool.h",
    "-DPAPP_C64=1",
    "-DESP32=1",
    "-DFRODO_PC=1",
    "-DLSB_FIRST=1",
    "-DHAVE_USLEEP=1",
]

CXXFLAGS = [
    "-std=gnu++17",
    "-D_Static_assert=static_assert",
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
    includes = [str(HERE), str(HERE / "compat"), str(CORE)]
    per_file = {str(source): CXXFLAGS for source in SOURCES if source.suffix == ".cpp"}
    out_dir = ROOT / "build/apps/c64"
    papp_build.build(
        "c64",
        [str(source) for source in SOURCES],
        includes=includes,
        libc=True,
        out_dir=str(out_dir),
        app_cflags=DEFINES,
        per_file_cflags=per_file,
        extra_libs=["-lstdc++"],
        icon_dir=str(HERE),
        version_dir=str(HERE),
        jobs=os.cpu_count() or 4,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

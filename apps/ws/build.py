#!/usr/bin/env python3
"""Build Oswan WonderSwan and WonderSwan Color as a native PAPP2 app."""

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
    CORE / "WS.c",
    CORE / "WSFileio.c",
    CORE / "WSRender.c",
    CORE / "WSApu.c",
    CORE / "WSInput.c",
    CORE / "cpu/nec.c",
]

CXX_FLAGS = [
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
]

APP_CFLAGS = [
    "-O2",
    "-DPAPP_APP_SIDE=1",
    "-DWS_PAPP=1",
    "-DWS_NO_SPLASH_SCREEN=1",
    "-Wno-format",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-Wno-maybe-uninitialized",
    "-Wno-sign-compare",
    "-Wno-missing-braces",
    "-Wno-unused-but-set-variable",
]


def main() -> int:
    if not (SDK / "tools/papp_build.py").is_file():
        raise SystemExit(f"PAPP SDK not found at {SDK}")

    papp_build.build(
        "ws",
        [str(source) for source in SOURCES],
        includes=[str(HERE / "native"), str(HERE), str(CORE), str(CORE / "cpu")],
        libc=True,
        out_dir=str(ROOT / "build/apps/ws"),
        app_cflags=APP_CFLAGS,
        per_file_cflags={str(HERE / "native/papp2_main.cpp"): CXX_FLAGS},
        extra_libs=["-lstdc++"],
        icon_dir=str(HERE),
        version_dir=str(HERE / "native"),
        jobs=os.cpu_count() or 4,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

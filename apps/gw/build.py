#!/usr/bin/env python3
"""Build Game & Watch as a native PAPP2 application.

Run from the repository root with ``python apps/gw/build.py``. The output is
``build/apps/gw/gw.papp``.
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
    HERE / "native/papp2_main.c",
    CORE / "src/lz4_depack.c",
    CORE / "src/cpus/sm500core.c",
    CORE / "src/cpus/sm500op.c",
    CORE / "src/cpus/sm510base.c",
    CORE / "src/cpus/sm510core.c",
    CORE / "src/cpus/sm510op.c",
    CORE / "src/cpus/sm511core.c",
    CORE / "src/cpus/sm5acore.c",
    CORE / "src/gw_sys/gw_graphic.c",
    CORE / "src/gw_sys/gw_romloader.c",
    CORE / "src/gw_sys/gw_system.c",
]

APP_CFLAGS = [
    "-O2",
    "-std=gnu99",
    "-DPAPP_APP_SIDE=1",
    "-DGW_PAPP=1",
    "-DGW_PAPP_NATIVE=1",
    "-DIRAM_ATTR=",
    "-Wno-format",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-Wno-maybe-uninitialized",
    "-Wno-implicit-function-declaration",
    "-Wno-pointer-sign",
    "-Wno-int-conversion",
    "-Wno-incompatible-pointer-types",
    "-Wno-sign-compare",
    "-Wno-missing-braces",
]


def main() -> int:
    if not (SDK / "tools/papp_build.py").is_file():
        raise SystemExit(f"PAPP SDK not found at {SDK}")

    papp_build.build(
        "gw",
        [str(source) for source in SOURCES],
        includes=[
            str(HERE / "native"),
            str(HERE),
            str(CORE),
            str(CORE / "src"),
            str(CORE / "src/cpus"),
            str(CORE / "src/gw_sys"),
        ],
        libc=True,
        out_dir=str(ROOT / "build/apps/gw"),
        app_cflags=APP_CFLAGS,
        icon_dir=str(HERE),
        version_dir=str(HERE / "native"),
        jobs=os.cpu_count() or 4,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

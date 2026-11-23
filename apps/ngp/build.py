#!/usr/bin/env python3
"""Build Neo Geo Pocket / Pocket Color as a native PAPP2 app."""

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

SOURCES = [HERE / "native/papp2_main.c", *sorted(CORE.glob("*.c"))]
APP_CFLAGS = [
    "-O2",
    "-std=gnu11",
    "-DPAPP_APP_SIDE=1",
    "-DNGP_PAPP=1",
    "-DCZ80=1",
    "-DUSE_CZ80=1",
    "-DIRAM_ATTR=",
    "-include",
    "stdlib.h",
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
    "-Wno-unused-but-set-variable",
]


def main() -> int:
    if not (SDK / "tools/papp_build.py").is_file():
        raise SystemExit(f"PAPP SDK not found at {SDK}")

    papp_build.build(
        "ngp",
        [str(source) for source in SOURCES],
        includes=[str(HERE / "native"), str(HERE), str(CORE)],
        libc=True,
        out_dir=str(ROOT / "build/apps/ngp"),
        app_cflags=APP_CFLAGS,
        icon_dir=str(HERE),
        version_dir=str(HERE / "native"),
        jobs=os.cpu_count() or 4,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

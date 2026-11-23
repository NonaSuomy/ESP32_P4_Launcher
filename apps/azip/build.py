#!/usr/bin/env python3
"""Build AZIP, a Z-machine interpreter, as a native PAPP2 app."""
from __future__ import annotations

import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CORE = HERE / "core"
sys.path.insert(0, str(ROOT / "papp-sdk" / "tools"))
import papp_build  # noqa: E402

SOURCES = [
    HERE / "native/papp2_main.cpp",
    *[CORE / f"{name}.cpp" for name in (
        "configur", "control", "extern", "fileio", "input", "interpre",
        "math", "object", "operand", "osdepend", "property", "screen",
        "text", "variable",
    )],
]
CXX_FLAGS = [
    "-std=gnu++17", "-D_Static_assert=static_assert", "-fno-exceptions",
    "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-Wno-write-strings", "-Wno-narrowing",
]


def main() -> int:
    papp_build.build(
        "azip", [str(path) for path in SOURCES],
        includes=[str(HERE / "native"), str(HERE), str(CORE)],
        libc=True,
        app_cflags=["-O2", "-DPAPP_APP_SIDE=1", "-Wno-format",
                    "-Wno-unused-variable", "-Wno-unused-function",
                    "-Wno-maybe-uninitialized"],
        per_file_cflags={str(HERE / "native/papp2_main.cpp"): CXX_FLAGS},
        extra_libs=["-lstdc++"], icon_dir=str(HERE), version_dir=str(HERE),
        out_dir=str(ROOT / "build/apps/azip"), jobs=os.cpu_count() or 4,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

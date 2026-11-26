#!/usr/bin/env python3
"""Build BLE Input Test as a native PAPP2 app with the pinned LVGL sources."""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

APP = Path(__file__).resolve().parent
ROOT = APP.parents[1]
LVGL = Path.home() / "code/pax/pax-modules/.cache/lvgl-85aa60d18b3d"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk", type=Path, default=ROOT / "papp-sdk")
    ap.add_argument("--abi", type=Path, default=ROOT / "components/papp_abi/include")
    ap.add_argument("--lvgl-dir", type=Path, default=LVGL)
    ap.add_argument("--out", type=Path,
                    default=ROOT / "community/ble-input-test/ble-input-test.papp")
    ap.add_argument("--build-dir", type=Path,
                    default=Path.home() / "code/pax/pax-dev/build/ble-input-test-papp2")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()
    sdk, abi, lvgl = a.sdk.resolve(), a.abi.resolve(), a.lvgl_dir.resolve()
    if not (sdk / "tools/papp_build.py").is_file():
        ap.error(f"PAPP2 SDK missing at {sdk}")
    if not (abi / "papp_abi.h").is_file():
        ap.error(f"PAPP2 ABI missing at {abi}")
    if not (lvgl / "src/lvgl.h").is_file():
        ap.error(f"Pinned LVGL source missing at {lvgl}")

    sources = [APP / "main.c"]
    sources.extend(c for c in sorted((lvgl / "src").rglob("*.c"))
                   if c.relative_to(lvgl / "src").parts[0] != "drivers")
    sys.path.insert(0, str(sdk / "tools"))
    from papp_build import build
    _elf, built_image = build("ble-input-test", [str(p) for p in sources],
                   includes=[str(abi), str(APP), str(lvgl), str(lvgl / "src")],
                   libc=True, out_dir=str(a.build_dir.resolve()),
                   app_cflags=["-O2", "-DPAPP_APP_SIDE=1", "-DLV_CONF_INCLUDE_SIMPLE=1",
                               "-Wno-unused-function", "-Wno-unused-variable"],
                   icon_dir=str(APP), version_dir=str(APP), jobs=a.jobs)
    out = a.out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(built_image, out)
    print(f"Native PAPP2 BLE Input Test: {out} ({out.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

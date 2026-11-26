#!/usr/bin/env python3
"""Build both ScummVM engines as native PAPP2 apps.

Set SCUMMVM_ROOT and SCUMMVM_DEPS_LIB to a configured RISC-V ScummVM build
and its dependency archives before running apps/build_all.py.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILDER = HERE / "tools/build_scummvm_papp2.py"

for engine in ("agi", "sci"):
    result = subprocess.run([sys.executable, str(BUILDER), engine], env=os.environ.copy())
    if result.returncode:
        raise SystemExit(result.returncode)

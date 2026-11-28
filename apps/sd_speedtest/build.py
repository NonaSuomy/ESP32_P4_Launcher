#!/usr/bin/env python3
"""Build the native PAPP2 SD card speed and readback test."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

COMMON = os.path.join(ROOT, "apps", "education", "common")
papp_build.build(
    "sd_speedtest",
    [os.path.join(HERE, "main.c"), os.path.join(COMMON, "edu_ui.c")],
    libc=True,
    includes=[COMMON],
    out_dir=os.path.join(ROOT, "build", "apps", "sd_speedtest"),
)

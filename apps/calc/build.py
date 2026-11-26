#!/usr/bin/env python3
"""Calculator app (uses the education apps' touch UI, apps/education/common/edu_ui.c).
    python apps/calc/build.py          -> build/apps/calc/calc.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

COMMON = os.path.join(HERE, "..", "education", "common")
papp_build.build("calc", [os.path.join(HERE, "main.c"), os.path.join(COMMON, "edu_ui.c")],
                 libc=True, includes=[COMMON])

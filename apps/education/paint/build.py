#!/usr/bin/env python3
"""Touch Paint (education).
    python apps/education/paint/build.py  -> SDcard/apps/Education/paint/paint.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("paint", ["main.c"], libc=True)

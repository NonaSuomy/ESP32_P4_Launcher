#!/usr/bin/env python3
"""Game of Life (education).
    python apps/education/life/build.py  -> SDcard/apps/Education/life/life.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("life", ["main.c"], libc=True)

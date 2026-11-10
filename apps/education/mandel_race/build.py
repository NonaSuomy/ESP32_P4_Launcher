#!/usr/bin/env python3
"""Mandelbrot Race (education).
    python apps/education/mandel_race/build.py  -> SDcard/apps/Education/mandel_race/mandel_race.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("mandel_race", ["main.c"], libc=True)

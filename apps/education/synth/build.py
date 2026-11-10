#!/usr/bin/env python3
"""Synth (education).
    python apps/education/synth/build.py  -> SDcard/apps/Education/synth/synth.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("synth", ["main.c"], libc=True)

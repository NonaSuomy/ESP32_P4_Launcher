#!/usr/bin/env python3
"""Sprite Demo (education).
    python apps/education/sprite_demo/build.py  -> SDcard/apps/Education/sprite_demo/sprite_demo.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("sprite_demo", ["main.c"], libc=True)

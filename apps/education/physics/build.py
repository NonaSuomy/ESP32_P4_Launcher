#!/usr/bin/env python3
"""Physics Playground (education).
    python apps/education/physics/build.py  -> SDcard/apps/Education/physics/physics.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("physics", ["main.c"], libc=True)

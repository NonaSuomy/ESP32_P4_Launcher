#!/usr/bin/env python3
"""BLE Remote (education).
    python apps/education/ble_remote/build.py  -> SDcard/apps/Education/ble_remote/ble_remote.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("ble_remote", ["main.c"], libc=True)

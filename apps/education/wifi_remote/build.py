#!/usr/bin/env python3
"""Wi-Fi Remote (education).
    python apps/education/wifi_remote/build.py  -> SDcard/apps/Education/wifi_remote/wifi_remote.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

edubuild.build("wifi_remote", ["main.c"], libc=True)

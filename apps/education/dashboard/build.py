#!/usr/bin/env python3
"""Sensor Dashboard (education): links its own LVGL 9.2.2 (launcher/managed_components; lv_conf.h here).
    python apps/education/dashboard/build.py  -> SDcard/apps/Education/dashboard/dashboard.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

kw = {"libc": True}
edubuild.build("dashboard", edubuild.lvgl(["main.c"], kw), **kw)

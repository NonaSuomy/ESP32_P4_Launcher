#!/usr/bin/env python3
"""LVGL Demo (education): links its own LVGL 9.2.2 (launcher/managed_components; lv_conf.h here).
    python apps/education/lvgl_demo/build.py  -> SDcard/apps/Education/lvgl_demo/lvgl_demo.papp"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import edubuild  # noqa: E402

kw = {"libc": True}
edubuild.build("lvgl_demo", edubuild.lvgl(["main.c"], kw), **kw)

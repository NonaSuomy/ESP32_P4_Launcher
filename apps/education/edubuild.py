"""Builds an education app and puts it on the starter card.

Education apps are for students: they go into the Education folder of the home screen
(SDcard/apps/Education/<dir>/, committed to git, copied to the SD card as it is) and never
into the Store catalogue. Their objects live in build/education/<dir>, which
tools/make_apps_release.py doesn't look at.

Every education app is linked with common/edu_ui.c (touch UI, log panel, the remotes' Control
screen; see common/edu_ui.h). An app's build.py:
    import edubuild
    edubuild.build("ble_remote", ["main.c"], libc=True)      # sources relative to the app's folder
An app with LVGL (its own copy, linked in; lv_conf.h beside main.c):
    kw = {"libc": True}
    edubuild.build("lvgl_demo", edubuild.lvgl(["main.c"], kw), **kw)
"""
import inspect
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

FOLDER = "Education"                                   # its name on the home screen
CARD = os.path.join(ROOT, "SDcard", "apps", FOLDER)
FOLDER_ICON = os.path.join(ROOT, "apps", "common", "icons", "folder_education.png")


LVGL_DIR = os.path.join(ROOT, "launcher", "managed_components", "lvgl__lvgl")
LVGL_SKIP = ("drivers", "libs/", "draw/nxp", "draw/renesas", "draw/sdl", "draw/vg_lite", "others/vg_lite_tvg",
             "stdlib/micropython", "stdlib/rtthread", "osal/lv_")    # not used: GPUs, OS ports, image libraries
LVGL_KEEP = ("libs/bin_decoder/",)            # lv_init needs LVGL's own image format


def lvgl(sources, kw):
    """Adds LVGL (the launcher's copy, 9.2.2, from launcher/managed_components) to an app's build.
    lv_conf.h goes beside the app's sources. Returns the sources with LVGL's appended."""
    src = os.path.join(LVGL_DIR, "src")
    if not os.path.isdir(src):
        sys.exit("LVGL not found: run `idf.py reconfigure` in launcher/ once (it fetches LVGL 9.2.2)")
    lv = []
    for d, _, files in os.walk(src):
        rel = os.path.relpath(d, src).replace(os.sep, "/")
        for f in sorted(files):
            path = (rel + "/" + f) if rel != "." else f
            if f.endswith(".c") and (path.startswith(LVGL_KEEP)
                                     or not any(path.startswith(s) for s in LVGL_SKIP if not s.startswith("osal/"))) \
                    and not (path.startswith("osal/lv_") and f not in ("lv_os.c", "lv_os_none.c")):
                lv.append(os.path.join(d, f))
    app_dir = os.path.dirname(os.path.abspath(inspect.stack()[1].filename))
    kw["includes"] = list(kw.get("includes", ())) + [app_dir, LVGL_DIR]
    kw["extra_cflags"] = list(kw.get("extra_cflags", ())) + ["-DLV_CONF_INCLUDE_SIMPLE", "-DLV_KCONFIG_IGNORE"]
    pf = dict(kw.get("per_file_cflags") or {})
    for f in lv:
        pf[f] = pf.get(f, []) + ["-w"]          # LVGL's own warnings are not ours
    kw["per_file_cflags"] = pf
    return list(sources) + lv


def build(name, sources, **kw):
    app_dir = os.path.dirname(os.path.abspath(inspect.stack()[1].filename))
    out = os.path.join(ROOT, "build", "education", name)
    common = os.path.join(HERE, "common")                # edu_ui: every education app gets it
    srcs = [os.path.join(app_dir, s) for s in sources] + [os.path.join(common, "edu_ui.c")]
    kw["includes"] = list(kw.get("includes", ())) + [common]
    papp_build.build(name, srcs, out_dir=out, **kw)

    dest = os.path.join(CARD, name)
    os.makedirs(dest, exist_ok=True)
    shutil.copyfile(os.path.join(out, name + ".papp"), os.path.join(dest, name + ".papp"))
    if os.path.isfile(os.path.join(out, "icon.png")):
        shutil.copyfile(os.path.join(out, "icon.png"), os.path.join(dest, "icon.png"))
    shutil.copyfile(FOLDER_ICON, os.path.join(CARD, "icon.png"))
    print(f"{name}: copied to {os.path.relpath(dest, ROOT)}")

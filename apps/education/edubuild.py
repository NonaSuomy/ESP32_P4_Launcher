"""Builds an education app and puts it on the starter card.

Education apps are for students: they go into the Education folder of the home screen
(SDcard/apps/Education/<dir>/, committed to git, copied to the SD card as it is) and never
into the Store catalogue. Their objects live in build/education/<dir>, which
tools/make_apps_release.py doesn't look at.

Every education app is linked with common/edu_ui.c (touch UI, log panel, the remotes' Control
screen; see common/edu_ui.h). An app's build.py:
    import edubuild
    edubuild.build("ble_remote", ["main.c"], libc=True)      # sources relative to the app's folder
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

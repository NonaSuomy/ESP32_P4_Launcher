#!/usr/bin/env python3
"""
Prepares the online app catalogue for the launcher's Store: copies every built app that
is not a developer app (PAPP_MF_HIDDEN), its icon and the folder icons, and writes
apps.json next to them. --publish uploads them to the "apps" release on GitHub.

    python apps/build_all.py                       # build the apps first
    python tools/make_apps_release.py [--out build/apps_release] [--publish]

The "apps" release is a pre-release, so it never becomes releases/latest (where the
launcher looks for its own update); its files are replaced in place on each publish.
An app's version is its manifest's (the app's version.txt); the launcher offers an
update when the catalogue's is newer than the installed one.

apps.json:
  {"format": 1,
   "folders": {"Emulators": {"icon": url}, ...},
   "apps": [{"id", "name", "version", "folder", "dir", "file", "url", "size", "sha256", "icon",
             "category", "description"}],
   "gamepads": {"url", "size", "sha256"},
   "community": [{the app fields, plus "author", "license", "source"}]}
category and description: the Store's (launcher 0.26+), from apps/store.json for the project's
apps and from app.json for community ones (category optional there). category is emulators,
games, tools, productivity or education.
folder: where a new install goes (/sd/apps/<folder>/<dir>/<file>; "" = the main page).
An installed app is updated where it is, wherever the user moved it.
gamepads: the controller mapping database, SDcard/config/gamepads.txt; the launcher
replaces /sd/config/gamepads.txt with it when the Store reads the catalogue.
community: the apps under community/ (shared without source, checked by tools/community.py),
in the "Community" folder. Their files are uploaded as c_<dir>.papp and c_<dir>.png, or the
app's own url is used. Launchers before 0.25 ignore this list.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "giltal/ESP32_P4_Launcher"
TAG = "apps"
BASE = f"https://github.com/{REPO}/releases/download/{TAG}/"
ICONS = os.path.join(ROOT, "apps", "common", "icons")
GAMEPADS = os.path.join(ROOT, "SDcard", "config", "gamepads.txt")
STORE = os.path.join(ROOT, "apps", "store.json")
CATEGORIES = ("emulators", "games", "tools", "productivity", "education")
FOLDERS = {"emu.": "Emulators", "game.": "Games"}          # by app id prefix; others: main page
FOLDER_ICONS = {"Emulators": "folder_emulators.png", "Games": "folder_games.png",
                "Community": "folder_community.png"}
COMMUNITY_FOLDER = "Community"
PAPP_MAGIC = 0x32504150          # "PAP2"
MANIFEST_MAGIC = 0x464E414D      # "MANF"
MF_HIDDEN = 1 << 1


def manifest(path):
    """id, name, version, flags of a .papp, or None."""
    data = open(path, "rb").read()
    magic, = struct.unpack_from("<I", data, 0)
    header_size, = struct.unpack_from("<H", data, 6)
    manifest_off, = struct.unpack_from("<I", data, 32)
    if magic != PAPP_MAGIC or not manifest_off:
        return None
    m = data[header_size + manifest_off:header_size + manifest_off + 96]
    if struct.unpack_from("<I", m, 0)[0] != MANIFEST_MAGIC:
        return None
    cstr = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")
    return dict(id=cstr(m[8:40]), name=cstr(m[40:72]), version=cstr(m[72:88]),
                flags=struct.unpack_from("<I", m, 92)[0])


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "apps_release"))
    ap.add_argument("--publish", action="store_true", help=f"upload to the '{TAG}' release with gh")
    ap.add_argument("--base", default=BASE, help="where the files will be (to test from another server, "
                    "named in /sd/update/apps_url.txt on the card)")
    args = ap.parse_args()
    base = args.base if args.base.endswith("/") else args.base + "/"
    os.makedirs(args.out, exist_ok=True)
    for old in os.listdir(args.out):                # emptied, not removed: a test server may serve it
        if os.path.isfile(os.path.join(args.out, old)):
            os.remove(os.path.join(args.out, old))

    store = json.load(open(STORE, encoding="utf-8"))
    apps, files, folders = [], [], {}
    built = os.path.join(ROOT, "build", "apps")
    for name in sorted(os.listdir(built)):
        papp = os.path.join(built, name, name + ".papp")
        if not os.path.isfile(papp):
            continue
        m = manifest(papp)
        if not m:
            print(f"skip {name}: no manifest")
            continue
        if m["flags"] & MF_HIDDEN:
            continue
        folder = next((f for p, f in FOLDERS.items() if m["id"].startswith(p)), "")
        entry = dict(id=m["id"], name=m["name"], version=m["version"], folder=folder, dir=name,
                     file=name + ".papp", url=base + name + ".papp", size=os.path.getsize(papp),
                     sha256=sha256(papp))
        info = store.get(m["id"])
        if not info or info.get("category") not in CATEGORIES or not info.get("description"):
            raise SystemExit(f"{m['id']}: add its category and description to apps/store.json")
        if len(info["description"]) > 120:
            raise SystemExit(f"{m['id']}: description in apps/store.json is longer than 120 characters")
        entry["category"], entry["description"] = info["category"], info["description"]
        shutil.copyfile(papp, os.path.join(args.out, name + ".papp"))
        files.append(name + ".papp")
        icon = os.path.join(built, name, "icon.png")
        if os.path.isfile(icon):
            shutil.copyfile(icon, os.path.join(args.out, name + ".png"))
            files.append(name + ".png")
            entry["icon"] = base + name + ".png"
        apps.append(entry)
        if folder and folder not in folders:
            folders[folder] = {}
            fi = FOLDER_ICONS.get(folder)
            if fi and os.path.isfile(os.path.join(ICONS, fi)):
                shutil.copyfile(os.path.join(ICONS, fi), os.path.join(args.out, fi))
                files.append(fi)
                folders[folder]["icon"] = base + fi
        print(f"{m['name']:16s} {m['version']:8s} {folder or '(main page)'}")
    if not apps:
        raise SystemExit("no app built: run python apps/build_all.py")

    def add_folder_icon(folder):
        if folder in folders:
            return
        folders[folder] = {}
        fi = FOLDER_ICONS.get(folder)
        if fi and os.path.isfile(os.path.join(ICONS, fi)):
            shutil.copyfile(os.path.join(ICONS, fi), os.path.join(args.out, fi))
            files.append(fi)
            folders[folder]["icon"] = base + fi

    community = []
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import community as cm
    for folder in cm.folders():
        app, errors = cm.load(folder)
        if errors:
            raise SystemExit(f"community/{app['dir']}: " + "; ".join(errors) + " (python tools/community.py)")
        if app["id"] in {a["id"] for a in apps}:
            raise SystemExit(f"community/{app['dir']}: id {app['id']} is one of the project's apps")
        meta = app["meta"]
        entry = dict(id=app["id"], name=app["name"], version=app["version"], folder=COMMUNITY_FOLDER,
                     dir=app["dir"], file=app["papp_file"], size=app["size"], sha256=app["sha256"],
                     author=meta["author"].strip(), description=meta["description"].strip(),
                     license=meta["license"].strip())
        if meta.get("source"):
            entry["source"] = meta["source"]
        if meta.get("category"):
            entry["category"] = meta["category"]
        if app.get("path"):
            asset = f"c_{app['dir']}.papp"
            shutil.copyfile(app["path"], os.path.join(args.out, asset))
            files.append(asset)
            entry["url"] = base + asset
        else:
            entry["url"] = app["url"]
        asset = f"c_{app['dir']}.png"
        shutil.copyfile(os.path.join(folder, "icon.png"), os.path.join(args.out, asset))
        files.append(asset)
        entry["icon"] = base + asset
        community.append(entry)
        add_folder_icon(COMMUNITY_FOLDER)
        print(f"{app['name']:16s} {app['version']:8s} community, by {entry['author']}")

    catalogue = dict(format=1, folders=folders, apps=apps)
    if community:
        catalogue["community"] = community
    if os.path.isfile(GAMEPADS):
        shutil.copyfile(GAMEPADS, os.path.join(args.out, "gamepads.txt"))
        files.append("gamepads.txt")
        catalogue["gamepads"] = dict(url=base + "gamepads.txt", size=os.path.getsize(GAMEPADS),
                                     sha256=sha256(GAMEPADS))
        pads = sum(1 for line in open(GAMEPADS) if line.strip() and not line.lstrip().startswith("#"))
        print(f"gamepads.txt: {pads} controllers")
    with open(os.path.join(args.out, "apps.json"), "w") as f:
        json.dump(catalogue, f, indent=1)
    files.append("apps.json")
    print(f"{len(apps)} apps and {len(community)} community apps in {args.out}")

    paths = [os.path.join(args.out, n) for n in files]
    if not args.publish:
        print(f"\npublish with: python tools/make_apps_release.py --publish")
        return
    exists = subprocess.run(["gh", "release", "view", TAG, "-R", REPO], capture_output=True).returncode == 0
    if not exists:
        subprocess.run(["gh", "release", "create", TAG, "-R", REPO, "--prerelease", "--title", "Apps",
                        "--notes", "Apps for the Store on the launcher. Replaced in place on each "
                        "publish; the launcher compares versions."], check=True)
    subprocess.run(["gh", "release", "upload", TAG, "-R", REPO, "--clobber"] + paths, check=True)
    print(f"published {len(apps)} apps to {BASE}apps.json")


if __name__ == "__main__":
    sys.exit(main())

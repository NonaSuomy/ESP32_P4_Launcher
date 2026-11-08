#!/usr/bin/env python3
"""
Community apps: .papp files shared without their source, one folder each under community/.
Checks every folder (the pull request check runs this) and reads them for the app catalogue
(tools/make_apps_release.py).

    python tools/community.py                     # check every app
    python tools/community.py --base origin/main  # also: a changed app has a higher version

community/<dir>/
  <file>.papp      the app, built with the SDK (or "url" + "sha256" + "size" in app.json)
  icon.png         square PNG, 64 to 256 pixels (88x88 is best), transparency OK
  app.json         {"author", "description", "license", "source" (optional),
                    "category" (optional: games, tools, productivity, education or emulators; the Store also
                    lists the app there, besides Community)}
  screenshot.png   optional

Folders whose name starts with "_" (community/_template) are skipped.
"""
CATEGORIES = ("games", "tools", "productivity", "education", "emulators")

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMUNITY = os.path.join(ROOT, "community")
ABI_H = os.path.join(ROOT, "components", "papp_abi", "include", "papp_abi.h")

MAX_PAPP = 4 * 1000 * 1000              # bytes, for a .papp kept in the repository
MAX_ICON = 64 * 1000
MAX_SCREENSHOT = 500 * 1000
RESERVED = ("emu.", "game.", "test.", "sys.", "lx.")   # ids of the project's own apps
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{2,30}$")
DIR_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,22}$")
FILE_RE = re.compile(r"^[A-Za-z0-9_-]{1,26}\.papp$")
VER_RE = re.compile(r"^\d+\.\d+\.\d+$")

PAPP_MAGIC = 0x32504150         # "PAP2"
MANIFEST_MAGIC = 0x464E414D     # "MANF"
MF_HIDDEN = 1 << 1


def fourcc(v):
    return struct.pack("<I", v).decode("ascii", "replace").strip()


def abi():
    """Format version and {interface fourcc: version} of this launcher, from papp_abi.h."""
    h = open(ABI_H, encoding="utf-8").read()
    fmt = int(re.search(r"#define PAPP2_FMT_VERSION\s+(\d+)", h).group(1))
    ifaces = {}
    for name, a, b, c, d in re.findall(r"#define PAPP_IF_(\w+)\s+PAPP_FOURCC\('(.)',\s*'(.)',\s*'(.)',\s*'(.)'\)", h):
        ver = re.search(rf"#define PAPP_IF_{name}_VER\s+(\d+)", h)
        ifaces[(a + b + c + d).strip()] = int(ver.group(1)) if ver else 1
    return fmt, ifaces


def read_papp(data):
    """Header fields, manifest and imports of a .papp (raises ValueError)."""
    if len(data) < 64:
        raise ValueError("too short to be a .papp")
    magic, fmt, header_size = struct.unpack_from("<IHH", data, 0)
    if magic != PAPP_MAGIC:
        raise ValueError("not a .papp v2 file (wrong magic)")
    (image_size, bss, entry, imports_off, imports_count,
     manifest_off) = struct.unpack_from("<IIIIII", data, 12)
    if not manifest_off:
        raise ValueError("no manifest: build it with papp_build.py (PAPP_APP in the source)")
    m = data[header_size + manifest_off:header_size + manifest_off + 96]
    if len(m) < 96 or struct.unpack_from("<I", m, 0)[0] != MANIFEST_MAGIC:
        raise ValueError("manifest not found")
    cstr = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")
    manifest = dict(id=cstr(m[8:40]), name=cstr(m[40:72]), version=cstr(m[72:88]),
                    flags=struct.unpack_from("<I", m, 92)[0])
    imports = []
    for i in range(imports_count):
        off = header_size + imports_off + i * 12
        if off + 8 > len(data):
            raise ValueError("import table outside the file")
        iid, min_ver, flags = struct.unpack_from("<IHH", data, off)
        imports.append((fourcc(iid), min_ver, flags))
    return dict(fmt=fmt, manifest=manifest, imports=imports)


def png_size(path):
    head = open(path, "rb").read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", head[16:24])


def version_tuple(v):
    return tuple(int(x) for x in v.split("."))


def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": "ESP32_P4_Launcher-community-check"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def load(folder, fetch=True):
    """One community app: {dir, path, meta, data, papp_file, url, ...} and the problems found."""
    name = os.path.basename(folder)
    app, errors = dict(dir=name, folder=folder), []
    files = os.listdir(folder)
    meta = {}
    if "app.json" not in files:
        errors.append("app.json is missing")
    else:
        try:
            meta = json.load(open(os.path.join(folder, "app.json"), encoding="utf-8"))
            if not isinstance(meta, dict):
                raise ValueError("not an object")
        except ValueError as e:
            errors.append(f"app.json is not valid JSON: {e}")
            meta = {}
    app["meta"] = meta
    papps = [f for f in files if f.endswith(".papp")]
    data = None
    if len(papps) > 1:
        errors.append("more than one .papp in the folder")
    elif papps:
        app["papp_file"] = papps[0]
        app["path"] = os.path.join(folder, papps[0])
        data = open(app["path"], "rb").read()
        if len(data) > MAX_PAPP:
            errors.append(f"{papps[0]} is {len(data)} bytes, over {MAX_PAPP}: give \"url\", \"sha256\" and "
                          "\"size\" in app.json instead and keep the file out of the folder")
    elif meta.get("url"):
        url, sha, size = meta.get("url"), meta.get("sha256", ""), meta.get("size")
        app["url"] = url
        file = url.rsplit("/", 1)[-1]
        app["papp_file"] = file
        if not url.startswith("https://") or not file.endswith(".papp"):
            errors.append("url must be an https:// link to a .papp file")
        if not re.fullmatch(r"[0-9a-f]{64}", str(sha)) or not isinstance(size, int):
            errors.append("with url, app.json needs sha256 (64 lowercase hex) and size (bytes)")
        elif fetch and url.startswith("https://"):
            try:
                data = download(url)
            except Exception as e:
                errors.append(f"cannot download {url}: {e}")
            if data is not None and (hashlib.sha256(data).hexdigest() != sha or len(data) != size):
                errors.append("the file at url does not match sha256 / size in app.json")
                data = None
    else:
        errors.append("no .papp in the folder (or url in app.json)")
    app["data"] = data

    if not DIR_RE.match(name):
        errors.append("folder name: lowercase letters, digits, - and _, at most 23 characters")
    if app.get("papp_file") and not FILE_RE.match(app["papp_file"]):
        errors.append(".papp file name: letters, digits, - and _, at most 31 characters with .papp")
    for key, limit in (("author", 31), ("description", 120), ("license", 40)):
        v = meta.get(key)
        if not isinstance(v, str) or not v.strip():
            errors.append(f"app.json: \"{key}\" is missing")
        elif len(v) > limit:
            errors.append(f"app.json: \"{key}\" is longer than {limit} characters")
    if "source" in meta and not str(meta["source"]).startswith("https://"):
        errors.append("app.json: \"source\" must be an https:// link")
    if "category" in meta and meta["category"] not in CATEGORIES:
        errors.append("app.json: \"category\" must be one of " + ", ".join(CATEGORIES))

    icon = os.path.join(folder, "icon.png")
    if not os.path.isfile(icon):
        errors.append("icon.png is missing")
    else:
        size = png_size(icon)
        if not size:
            errors.append("icon.png is not a PNG")
        elif size[0] != size[1] or not 64 <= size[0] <= 256:
            errors.append(f"icon.png is {size[0]}x{size[1]}: it must be square, 64 to 256 pixels (88x88 is best)")
        if os.path.getsize(icon) > MAX_ICON:
            errors.append(f"icon.png is over {MAX_ICON} bytes")
    shot = os.path.join(folder, "screenshot.png")
    if os.path.isfile(shot) and (not png_size(shot) or os.path.getsize(shot) > MAX_SCREENSHOT):
        errors.append(f"screenshot.png must be a PNG of at most {MAX_SCREENSHOT} bytes")
    extra = set(files) - {"app.json", "icon.png", "screenshot.png", "README.md"} - set(papps)
    if extra:
        errors.append("files that do not belong here: " + ", ".join(sorted(extra)))

    if data is not None:
        try:
            info = read_papp(data)
        except ValueError as e:
            errors.append(f"{app['papp_file']}: {e}")
        else:
            m = info["manifest"]
            app.update(id=m["id"], name=m["name"], version=m["version"], size=len(data),
                       sha256=hashlib.sha256(data).hexdigest())
            fmt, ifaces = abi()
            if info["fmt"] != fmt:
                errors.append(f"built for .papp format {info['fmt']}, the launcher reads {fmt}: rebuild it")
            if not ID_RE.match(m["id"]):
                errors.append(f"app id '{m['id']}': lowercase letters, digits, '.', '-', '_', 3 to 31 characters")
            elif m["id"].startswith(RESERVED):
                errors.append(f"app id '{m['id']}' starts with a prefix kept for the project's own apps "
                              f"({', '.join(RESERVED)})")
            if not m["name"].strip():
                errors.append("the app has no name (PAPP_APP)")
            if not VER_RE.match(m["version"]):
                errors.append(f"version '{m['version']}' is not x.y.z: put it in version.txt beside the source")
            if m["flags"] & MF_HIDDEN:
                errors.append("the app is marked hidden (PAPP_MF_HIDDEN, a developer app)")
            for iid, min_ver, flags in info["imports"]:
                if flags & 1:
                    continue                                    # optional import
                if iid not in ifaces:
                    errors.append(f"needs interface '{iid}', which the launcher does not have")
                elif min_ver > ifaces[iid]:
                    errors.append(f"needs {iid} version {min_ver}, the launcher has {ifaces[iid]}")
    return app, errors


def base_version(base, app):
    """Version of this app at git ref `base`, or None if it is new there."""
    rel = f"community/{app['dir']}"
    try:
        listing = subprocess.run(["git", "ls-tree", "--name-only", f"{base}:{rel}"], cwd=ROOT,
                                 capture_output=True, text=True, check=True).stdout.split()
    except subprocess.CalledProcessError:
        return None
    old = None
    papps = [f for f in listing if f.endswith(".papp")]
    if papps:
        old = subprocess.run(["git", "show", f"{base}:{rel}/{papps[0]}"], cwd=ROOT, capture_output=True).stdout
    elif "app.json" in listing:
        meta = json.loads(subprocess.run(["git", "show", f"{base}:{rel}/app.json"], cwd=ROOT,
                                         capture_output=True, text=True).stdout or "{}")
        if meta.get("sha256") == app.get("sha256"):
            return app.get("version")
        if meta.get("url"):
            old = download(meta["url"])
    try:
        return read_papp(old)["manifest"]["version"] if old else None
    except ValueError:
        return None


def folders():
    if not os.path.isdir(COMMUNITY):
        return []
    return [os.path.join(COMMUNITY, d) for d in sorted(os.listdir(COMMUNITY))
            if os.path.isdir(os.path.join(COMMUNITY, d)) and not d.startswith(("_", "."))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="git ref to compare with: a changed app needs a higher version")
    args = ap.parse_args()
    failed, ids = False, {}
    for folder in folders():
        app, errors = load(folder)
        if app.get("id"):
            if app["id"] in ids:
                errors.append(f"app id '{app['id']}' is already used by community/{ids[app['id']]}")
            ids[app["id"]] = app["dir"]
        if args.base and app.get("version") and not errors:
            old = base_version(args.base, app)
            if old and old != app["version"] and version_tuple(app["version"]) <= version_tuple(old):
                errors.append(f"version {app['version']} is not higher than {old}: bump version.txt")
            elif old == app["version"] and app.get("sha256") and base_changed(args.base, app):
                errors.append(f"the app changed but its version is still {old}: bump version.txt")
        label = f"{app.get('name') or app['dir']} {app.get('version', '')}".strip()
        if errors:
            failed = True
            print(f"FAIL community/{app['dir']} ({label})")
            for e in errors:
                print(f"  - {e}")
        else:
            print(f"ok   community/{app['dir']} ({label}, id {app['id']}, by {app['meta']['author']})")
    if not folders():
        print("no community apps")
    return 1 if failed else 0


def base_changed(base, app):
    """True if the app's file differs from the one at `base`."""
    rel = f"community/{app['dir']}"
    if app.get("path"):
        old = subprocess.run(["git", "show", f"{base}:{rel}/{app['papp_file']}"], cwd=ROOT, capture_output=True)
        return old.returncode != 0 or hashlib.sha256(old.stdout).hexdigest() != app["sha256"]
    old = subprocess.run(["git", "show", f"{base}:{rel}/app.json"], cwd=ROOT, capture_output=True, text=True)
    return old.returncode != 0 or json.loads(old.stdout or "{}").get("sha256") != app["sha256"]


if __name__ == "__main__":
    sys.exit(main())

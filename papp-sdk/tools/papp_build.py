#!/usr/bin/env python3
"""
papp_build.py — compile, link and pack a PAPP app with the bare riscv32-esp-elf toolchain.

  python papp-sdk/tools/papp_build.py --name hello --src apps/hello/main.c
  python papp-sdk/tools/papp_build.py --name libc_test --src apps/libc_test/main.c --libc

Output: build/apps/<name>/<name>.papp (+ .elf, .map). No ESP-IDF project needed.
An icon.png next to the first source file is copied to build/apps/<name>/ too
(shown on the launcher home screen at 88x88; deploy uploads it with the app).
The app's version comes from a version.txt ("1.2.0") next to the first source file,
or next to the build script that called build(); it reaches the manifest as
PAPP_APP_VERSION.
(Interim driver until the CMake toolchain of WORKPLAN Phase 4.2.)
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(SDK)
sys.path.insert(0, os.path.join(SDK, "tools"))
import papp_pack  # noqa: E402

ARCH = ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f"]
CFLAGS = ARCH + ["-mcmodel=medany", "-Os", "-g", "-ffunction-sections", "-fdata-sections",
                 "-fno-common", "-Wall", "-I" + os.path.join(SDK, "include"),
                 "-I" + os.path.join(ROOT, "components", "papp_abi", "include")]
WRAPS = ["malloc", "free", "calloc", "realloc", "memalign",
         "_malloc_r", "_free_r", "_calloc_r", "_realloc_r", "_memalign_r",
         "__retarget_lock_init", "__retarget_lock_init_recursive", "__retarget_lock_close",
         "__retarget_lock_close_recursive", "__retarget_lock_acquire",
         "__retarget_lock_acquire_recursive", "__retarget_lock_try_acquire",
         "__retarget_lock_try_acquire_recursive", "__retarget_lock_release",
         "__retarget_lock_release_recursive"]


def find_tool(name):
    exe = shutil.which(name)
    if exe:
        return exe
    pattern = os.path.join(os.path.expanduser("~"), ".espressif", "tools", "riscv32-esp-elf", "*",
                           "riscv32-esp-elf", "bin", name + (".exe" if os.name == "nt" else ""))
    hits = sorted(glob.glob(pattern))
    if not hits:
        sys.exit(f"papp_build: {name} not found (install ESP-IDF tools or add the toolchain to PATH)")
    return hits[-1]


def run(cmd):
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(f"papp_build: command failed: {' '.join(cmd[:3])} ...")


ICON_SIZE = 88       # what the home screen shows; other sizes are scaled to it


def copy_icon(src_dir, out_dir):
    """icon.png beside the sources -> build dir, with a size sanity check."""
    icon = os.path.join(src_dir, "icon.png")
    if not os.path.isfile(icon):
        return
    with open(icon, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        print(f"papp_build: warning: {icon} is not a PNG, skipped", file=sys.stderr)
        return
    w, h = struct.unpack(">II", head[16:24])
    if w != h or max(w, h) > 4 * ICON_SIZE:
        print(f"papp_build: warning: {icon} is {w}x{h}; {ICON_SIZE}x{ICON_SIZE} (square) is best "
              "(it is decoded at full size and kept in memory)", file=sys.stderr)
    shutil.copyfile(icon, os.path.join(out_dir, "icon.png"))


def app_version(sources, version_dir=None):
    """The version.txt of the app: version_dir, the first source's folder, or the
    folder of the build script that is running (emulators share their main file)."""
    dirs = [version_dir] if version_dir else [os.path.dirname(os.path.abspath(sources[0])),
                                              os.path.dirname(os.path.abspath(sys.argv[0]))]
    for d in dirs:
        path = os.path.join(d, "version.txt")
        if os.path.isfile(path):
            v = open(path, encoding="utf-8").read().strip()
            if not re.fullmatch(r"[0-9]+[.][0-9]+[.][0-9]+", v):
                sys.exit(f"papp_build: {path}: expected a version like 1.2.0, got {v!r}")
            return v
    return None


def build(name, sources, includes=(), libc=False, out_dir=None, link_base=0, extra_cflags=(),
          fast_link_base=0, libc_glue=True, app_cflags=(), per_file_cflags=None, extra_ldflags=(),
          icon_dir=None, jobs=None, extra_libs=(), version_dir=None):
    """extra_cflags: every file; app_cflags: the app's sources only (not the SDK
    runtime); per_file_cflags: {source: [flags]} appended last. libc_glue=False
    links newlib without the SDK's libc_glue.c (the app brings its own)."""
    gcc = find_tool("riscv32-esp-elf-gcc")
    out_dir = out_dir or os.path.join(ROOT, "build", "apps", name)
    obj_dir = os.path.join(out_dir, "obj")
    os.makedirs(obj_dir, exist_ok=True)

    rt = os.path.join(SDK, "runtime")
    runtime = [os.path.join(rt, "crt0.c"), os.path.join(rt, "papp_api.c")]
    if libc and libc_glue:
        runtime.append(os.path.join(rt, "libc_glue.c"))
    elif not libc:
        runtime.append(os.path.join(rt, "mini_libc.c"))

    cflags = CFLAGS + ["-I" + i for i in includes] + list(extra_cflags)
    if not libc:
        cflags += ["-ffreestanding", "-fno-tree-loop-distribute-patterns"]

    version = app_version(sources, version_dir)
    if version:
        app_cflags = list(app_cflags) + [f'-DPAPP_APP_VERSION="{version}"']
    per_file_cflags = per_file_cflags or {}
    jobs_list = []
    for i, src in enumerate(runtime + list(sources)):
        # unique object names: engines reuse file names in different folders
        obj = os.path.join(obj_dir, f"{i:04d}_" + os.path.splitext(os.path.basename(src))[0] + ".o")
        flags = cflags + (list(app_cflags) if src not in runtime else []) + per_file_cflags.get(src, [])
        jobs_list.append(([gcc] + flags + ["-c", src, "-o", obj], obj))
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=jobs or os.cpu_count() or 4) as pool:
        codes = list(pool.map(lambda j: subprocess.run(j[0]).returncode, jobs_list))
    failed = [j[0][-3] for j, c in zip(jobs_list, codes) if c]
    if failed:
        sys.exit(f"papp_build: {len(failed)} file(s) failed to compile, e.g. {failed[0]}")
    objs = [o for _, o in jobs_list]

    ld_script = os.path.join(SDK, "ld", "papp.ld")
    if link_base or fast_link_base:
        text = open(ld_script, encoding="utf-8").read().replace(". = 0;", f". = {link_base:#x};", 1)
        if fast_link_base:
            text = text.replace(".fast 0x10000000 :", f".fast {fast_link_base:#x} :", 1)
        ld_script = os.path.join(out_dir, "papp_base.ld")
        open(ld_script, "w", encoding="utf-8").write(text)

    elf = os.path.join(out_dir, name + ".elf")
    ldflags = ARCH + ["-nostartfiles", "-T", ld_script, "-Wl,--gc-sections", "-Wl,--no-relax",
                      "-Wl,-q", "-Wl,--no-warn-rwx-segments", "-Wl,-Map=" + elf[:-4] + ".map"]
    if libc:
        ldflags += ["-nodefaultlibs"] + ["-Wl,--wrap=" + w for w in WRAPS]
        libs = ["-Wl,--start-group", "-lc", "-lm", "-lgcc"] + list(extra_libs) + ["-Wl,--end-group"]
    else:
        ldflags += ["-nostdlib"]
        libs = ["-lgcc"]
    # Object list in a response file: hundreds of objects (LVGL) in a deep folder pass Windows' 32 KB command line
    rsp = os.path.join(out_dir, "objs.rsp")
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in objs) + "\n")
    run([gcc] + ldflags + list(extra_ldflags) + ["-o", elf, "@" + rsp] + libs)

    if link_base or fast_link_base:
        return elf, None
    papp = os.path.join(out_dir, name + ".papp")
    try:
        img = papp_pack.build_image(elf)
        open(papp, "wb").write(papp_pack.serialize(img))
    except papp_pack.PackError as e:
        sys.exit(f"papp_pack: error: {e}")
    copy_icon(icon_dir or os.path.dirname(os.path.abspath(sources[0])), out_dir)
    print(f"{name}: {papp} (v{version or ' none: no version.txt'}, image {len(img.image):,} B, bss {img.bss_size:,} B, "
          f"{len(img.relocs)} relocs, {img.imports_count} imports"
          + (f", FAST {len(img.fast):,} B + bss {img.fast_bss_size:,} B" if img.fast or img.fast_bss_size else "")
          + ")")
    return elf, papp


def main():
    ap = argparse.ArgumentParser(description="Build a PAPP app")
    ap.add_argument("--name", required=True)
    ap.add_argument("--src", nargs="+", required=True)
    ap.add_argument("--inc", nargs="*", default=[])
    ap.add_argument("--libc", action="store_true", help="link newlib (stdio, malloc, math)")
    ap.add_argument("--out")
    ap.add_argument("--link-base", type=lambda s: int(s, 0), default=0,
                    help="link at this address instead of 0 (tests only, no .papp)")
    args = ap.parse_args()
    build(args.name, args.src, args.inc, args.libc, args.out, args.link_base)


if __name__ == "__main__":
    main()

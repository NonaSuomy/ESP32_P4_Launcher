#!/usr/bin/env python3
"""h264.papp: the launcher's H.264 codec module (FFmpeg's decoder) -> build/codecs/h264/h264.papp.

  python apps/codecs/h264/build.py [--ffmpeg <FFmpeg 7.1.1 source>]

build_ffmpeg.py compiles FFmpeg's decoder into libffh264.a first (FFmpeg's source is not in the repo:
github.com/FFmpeg/FFmpeg n7.1.1, or ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz). Install on a board:
  python tools/papp_dev.py --port COMx put build/codecs/h264/h264.papp /sd/launcher/codecs/h264.papp
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

FFMPEG_VERSION = "7.1.1"


def default_ffmpeg():
    d = ROOT
    for _ in range(6):
        cand = os.path.join(d, "third_party_dl", "ffmpeg-" + FFMPEG_VERSION)
        if os.path.isdir(cand):
            return cand
        d = os.path.dirname(d)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ffmpeg", default=default_ffmpeg())
    ap.add_argument("-O", dest="opt", default="2", help="FFmpeg's optimisation level (2, 3)")
    args = ap.parse_args()
    if not args.ffmpeg or not os.path.isdir(args.ffmpeg):
        sys.exit("build.py: FFmpeg " + FFMPEG_VERSION + " source not found: pass --ffmpeg <dir>")
    out = os.path.join(ROOT, "build", "codecs", "h264")
    lib_dir = os.path.join(out, "ffmpeg_obj")
    r = subprocess.run([sys.executable, os.path.join(HERE, "build_ffmpeg.py"), "--ffmpeg", args.ffmpeg,
                        "--out", lib_dir, "-O", args.opt])
    if r.returncode:
        sys.exit("build.py: FFmpeg build failed")
    lib = os.path.join(lib_dir, "libffh264.a")
    includes = [os.path.join(HERE, "ffmpeg_config"), args.ffmpeg]
    papp_build.build("h264", [os.path.join(HERE, "h264_module.c"), os.path.join(HERE, "pthread_shim.c")],
                     includes=includes, libc=True, out_dir=out, module=True,
                     extra_cflags=["-O2"], app_cflags=['-DFFMPEG_VERSION_STR="' + FFMPEG_VERSION + '"'],
                     extra_libs=[lib], version_dir=HERE)


if __name__ == "__main__":
    main()

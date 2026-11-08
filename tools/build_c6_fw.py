#!/usr/bin/env python3
"""
Builds the ESP32-C6 Wi-Fi chip firmware (esp_hosted "network_adapter") that matches a board's launcher,
from the esp_hosted copy the launcher build downloaded, so both sides run the same version.

    python tools/build_c6_fw.py [--board guition_p4_7]      (in the ESP-IDF environment: tools/idf_env.ps1)

Result: build/c6_fw/c6_<version>.bin. Settings > Update installs it from /sd/update/c6.bin, and
tools/make_release.py adds it to the release for every board whose launcher speaks that version.
The board's launcher must have been configured last (idf.py ... reconfigure), so
launcher/managed_components holds its esp_hosted.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOSTED = os.path.join(ROOT, "launcher", "managed_components", "espressif__esp_hosted")
OUT = os.path.join(ROOT, "build", "c6_fw")


def pinned_version(board):
    """esp_hosted version a board pins in its idf_component.yml"""
    text = open(os.path.join(ROOT, "boards", board, "board", "idf_component.yml")).read()
    m = re.search(r'espressif/esp_hosted:\s*"?([0-9.]+)', text)
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="guition_p4_7")
    args = ap.parse_args()
    want = pinned_version(args.board)
    if not want:
        raise SystemExit(f"{args.board} doesn't use esp_hosted")
    have = re.search(r"^version:\s*([0-9.]+)", open(os.path.join(HOSTED, "idf_component.yml")).read(), re.M).group(1)
    if have != want:
        raise SystemExit(f"launcher/managed_components has esp_hosted {have}, {args.board} pins {want}: "
                         f"configure that board first (idf.py -B build_{args.board} -DLX_BOARD={args.board} reconfigure)")

    work = os.path.join(OUT, f"slave_{want}")
    if os.path.isdir(work):
        shutil.rmtree(work)
    shutil.copytree(os.path.join(HOSTED, "slave"), work)
    # The image's own version (esp_app_desc_t) is what the launcher checks before installing it;
    # esp_hosted 2.0 doesn't set it from its version file
    with open(os.path.join(work, "version.txt"), "w") as f:
        f.write(want + "\n")
    idf = shutil.which("idf.py") or shutil.which("idf.py.exe")
    cmd = [idf] if idf else [sys.executable, os.path.join(os.environ.get("IDF_PATH", ""), "tools", "idf.py")]
    subprocess.run(cmd + ["set-target", "esp32c6"], cwd=work, check=True)
    subprocess.run(cmd + ["build"], cwd=work, check=True)

    image = os.path.join(work, "build", "network_adapter.bin")
    data = open(image, "rb").read(400)
    version = data[48:80].split(b"\0")[0].decode()
    if data[0] != 0xE9 or (data[12] | data[13] << 8) != 13 or version != want:
        raise SystemExit(f"unexpected image: version {version!r}")
    dest = os.path.join(OUT, f"c6_{want}.bin")
    shutil.copyfile(image, dest)
    print(f"{dest}: {os.path.getsize(dest):,} bytes, esp_hosted {want} for the ESP32-C6")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Draws the Store tile icon (88x88 PNG): a white shopping bag with a download arrow on a
blue tile. Drawn at 4x and scaled down for smooth edges. The launcher has it built in
(launcher/main/CMakeLists.txt) and writes it to /sd/launcher/store.png.

    python apps/common/icons/make_store_icon.py
"""
import os
from PIL import Image, ImageDraw

S = 4
N = 88 * S
HERE = os.path.dirname(os.path.abspath(__file__))


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def main():
    img = Image.new("RGBA", (N, N))
    d = ImageDraw.Draw(img)
    for y in range(N):                                                       # blue to cyan tile
        d.line([(0, y), (N, y)], fill=lerp((59, 201, 245), (30, 111, 255), y / (N - 1)))
    white, blue = (255, 255, 255, 255), (36, 130, 250, 255)
    d.arc([30 * S, 12 * S, 58 * S, 40 * S], 180, 360, fill=white, width=4 * S)    # handle
    d.rounded_rectangle([18 * S, 26 * S, 70 * S, 74 * S], radius=8 * S, fill=white)   # bag
    cx = 44 * S                                                               # download arrow
    d.rectangle([cx - 4 * S, 34 * S, cx + 4 * S, 52 * S], fill=blue)
    d.polygon([(cx - 13 * S, 49 * S), (cx + 13 * S, 49 * S), (cx, 63 * S)], fill=blue)
    img.resize((88, 88), Image.LANCZOS).save(os.path.join(HERE, "store.png"))
    print("wrote store.png")


if __name__ == "__main__":
    main()

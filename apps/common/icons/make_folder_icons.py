#!/usr/bin/env python3
"""
Draws the home screen folder icons (88x88 PNG): an amber folder on a dark tile
with an emblem on its front. Drawn at 4x and scaled down for smooth edges.

    python apps/common/icons/make_folder_icons.py

Writes folder_games.png, folder_emulators.png, folder_community.png and folder_education.png next to this script. Copy one to
/sd/apps/<folder>/icon.png to use it.
"""
import os
from PIL import Image, ImageDraw

S = 4                       # supersampling
N = 88 * S
HERE = os.path.dirname(os.path.abspath(__file__))


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(w, h, top, bottom):
    g = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(g)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=lerp(top, bottom, y / max(h - 1, 1)))
    return g


def base():
    img = gradient(N, N, (30, 44, 82), (12, 20, 44)).convert("RGBA")
    # folder: back flap with a tab, then the front, both amber
    back = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(back)
    d.rounded_rectangle([10 * S, 17 * S, 40 * S, 29 * S], radius=4 * S, fill=255)       # tab
    d.rounded_rectangle([10 * S, 22 * S, 78 * S, 69 * S], radius=6 * S, fill=255)
    img.paste(gradient(N, N, (196, 120, 20), (150, 86, 10)), (0, 0), back)
    front = Image.new("L", (N, N), 0)
    ImageDraw.Draw(front).rounded_rectangle([8 * S, 30 * S, 80 * S, 71 * S], radius=6 * S, fill=255)
    img.paste(gradient(N, N, (255, 196, 84), (226, 146, 30)), (0, 0), front)
    return img


def gamepad(d, cx, cy, ink, hole):
    w, h = 42 * S, 20 * S
    x0, y0 = cx - w // 2, cy - h // 2
    d.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=10 * S, fill=ink)
    d.ellipse([x0 - 2 * S, y0 + 4 * S, x0 + 16 * S, y0 + h + 6 * S], fill=ink)                 # grips
    d.ellipse([x0 + w - 16 * S, y0 + 4 * S, x0 + w + 2 * S, y0 + h + 6 * S], fill=ink)
    px, py = x0 + 10 * S, cy                                                                     # d-pad
    d.rectangle([px - 6 * S, py - 2 * S, px + 6 * S, py + 2 * S], fill=hole)
    d.rectangle([px - 2 * S, py - 6 * S, px + 2 * S, py + 6 * S], fill=hole)
    bx, by = x0 + w - 10 * S, cy                                                                 # buttons
    for dx, dy in ((0, -4), (4, 0), (0, 4), (-4, 0)):
        r = 2 * S
        d.ellipse([bx + dx * S - r, by + dy * S - r, bx + dx * S + r, by + dy * S + r], fill=hole)


def cartridge(d, cx, cy, ink, hole):
    w, h = 30 * S, 30 * S
    x0, y0 = cx - w // 2, cy - h // 2
    d.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=3 * S, fill=ink)
    d.polygon([(x0 + w - 7 * S, y0), (x0 + w, y0), (x0 + w, y0 + 7 * S)], fill=hole)           # cut corner
    d.rounded_rectangle([x0 + 5 * S, y0 + 5 * S, x0 + w - 9 * S, y0 + 18 * S], radius=2 * S, fill=hole)   # label
    for i in range(6):                                                                           # contacts
        x = x0 + 4 * S + i * 4 * S
        d.rectangle([x, y0 + h - 7 * S, x + 2 * S, y0 + h - 2 * S], fill=hole)


def people(d, cx, cy, ink, hole):
    for dx, r, top in ((-11, 6, 3), (11, 6, 3), (0, 7, 0)):                                     # back two, front one
        x, y = cx + dx * S, cy - 9 * S + top * S
        if dx:
            d.ellipse([x - r * S, y - r * S, x + r * S, y + r * S], fill=ink)                    # head
            d.pieslice([x - 11 * S, y + 6 * S, x + 11 * S, y + 28 * S], 180, 360, fill=ink)       # shoulders
            continue
        d.ellipse([x - (r + 2) * S, y - (r + 2) * S, x + (r + 2) * S, y + (r + 2) * S], fill=hole)   # gap
        d.pieslice([x - 15 * S, y + 5 * S, x + 15 * S, y + 33 * S], 180, 360, fill=hole)
        d.ellipse([x - r * S, y - r * S, x + r * S, y + r * S], fill=ink)
        d.pieslice([x - 13 * S, y + 7 * S, x + 13 * S, y + 31 * S], 180, 360, fill=ink)


def cap(d, cx, cy, ink, hole):
    """Graduation cap: a board seen at an angle, the cap under it and a tassel."""
    d.polygon([(cx - 22 * S, cy - 6 * S), (cx, cy - 15 * S), (cx + 22 * S, cy - 6 * S), (cx, cy + 3 * S)], fill=ink)
    d.polygon([(cx - 12 * S, cy - 1 * S), (cx, cy + 4 * S), (cx + 12 * S, cy - 1 * S),
               (cx + 12 * S, cy + 9 * S), (cx, cy + 14 * S), (cx - 12 * S, cy + 9 * S)], fill=ink)
    d.line([(cx, cy - 6 * S), (cx + 17 * S, cy - 2 * S)], fill=hole, width=2 * S)                  # tassel cord
    d.line([(cx + 17 * S, cy - 2 * S), (cx + 17 * S, cy + 11 * S)], fill=ink, width=2 * S)
    d.ellipse([cx + 14 * S, cy + 9 * S, cx + 20 * S, cy + 15 * S], fill=ink)


def make(name, emblem):
    img = base()
    d = ImageDraw.Draw(img)
    emblem(d, N // 2, 51 * S, (255, 255, 255, 255), (226, 146, 30, 255))
    img.resize((88, 88), Image.LANCZOS).save(os.path.join(HERE, name))
    print("wrote", name)


if __name__ == "__main__":
    make("folder_games.png", gamepad)
    make("folder_emulators.png", cartridge)
    make("folder_community.png", people)
    make("folder_education.png", cap)

#!/usr/bin/env python3
"""gen_icon.py -- draws the Win.apk icon (launcher/winapk.ico and a PNG).

A rounded tile with a teal-to-blue gradient and a white phone outline that
holds a play mark: an app, running. Drawn at 1024 px and scaled down for each
icon size.

    windows/tools/gen_icon.py   (needs Pillow; rewrites windows/launcher/winapk.ico)
"""

import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "launcher")


def draw(size=1024):
    s = size
    grad = Image.new("RGBA", (s, s))
    top, bottom = (24, 200, 160), (30, 100, 230)
    px = grad.load()
    for y in range(s):
        for x in range(s):
            t = (x + y) / (2 * (s - 1))
            px[x, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.22), fill=255)
    tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    tile.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(tile)
    # The phone.
    w, h = s * 0.42, s * 0.66
    x0, y0 = (s - w) / 2, (s - h) / 2
    d.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=int(s * 0.07), outline=(255, 255, 255, 255),
                        width=int(s * 0.045))
    d.rounded_rectangle([s / 2 - s * 0.05, y0 + s * 0.045, s / 2 + s * 0.05, y0 + s * 0.07],
                        radius=int(s * 0.012), fill=(255, 255, 255, 255))
    # The play mark.
    cx, cy, r = s / 2 + s * 0.015, s / 2 + s * 0.02, s * 0.11
    d.polygon([(cx - r * 0.75, cy - r), (cx - r * 0.75, cy + r), (cx + r, cy)], fill=(255, 255, 255, 255))
    return tile


def main():
    big = draw()
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    big.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "winapk.ico"),
                                              sizes=[(n, n) for n in sizes])
    big.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "winapk.png"))
    print("[gen_icon] windows/launcher/winapk.ico, winapk.png")


if __name__ == "__main__":
    sys.exit(main())

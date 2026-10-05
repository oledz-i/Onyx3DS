#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the UWP splash screen for ONYX 3DS.

The splash is a static PNG shown by the OS while the app starts, so it costs
nothing at boot. Its edges fade to exactly the manifest BackgroundColor
(#101218) so the image blends into the full-screen fill with no visible box.
Run from the repo root:  python3 tools/gen_splash.py
Output: app/ONYX3DS/Assets/SplashScreen.png (620x300) and
        SplashScreen.scale-200.png (1240x600, used on Xbox at 1080p/4K)
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "app", "ONYX3DS", "Assets")
FONT_BOLD = os.path.join(ASSETS, "Fonts", "RoundedMplus1c-Bold.ttf")
FONT_MED = os.path.join(ASSETS, "Fonts", "RoundedMplus1c-Medium.ttf")

BG = np.array([16, 18, 24], dtype=np.float32)
VIOLET = (155, 123, 255)
CYAN = (120, 220, 240)
LAVENDER = (200, 190, 255)
FACE = (24, 22, 40)

S = 4                      # supersample factor over the 620x300 design grid
W, H = 620 * S, 300 * S


def glow(cx, cy, rx, ry, color, strength):
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
    a = np.exp(-d * 2.2) * strength
    return a[..., None] * (np.array(color, np.float32) - BG)


def edge_fade():
    """1 in the middle, smoothly 0 at the image border."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    fx = np.clip(np.minimum(x, W - 1 - x) / (W * 0.16), 0, 1)
    fy = np.clip(np.minimum(y, H - 1 - y) / (H * 0.22), 0, 1)
    f = fx * fy
    return (f * f * (3 - 2 * f))[..., None]


def dot_grid(spacing, radius, alpha):
    layer = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(layer)
    for gy in range(spacing // 2, H, spacing):
        for gx in range(spacing // 2, W, spacing):
            d.ellipse((gx - radius, gy - radius, gx + radius, gy + radius), fill=255)
    a = np.asarray(layer, np.float32) / 255.0 * alpha
    return a[..., None] * (np.array(LAVENDER, np.float32) - BG)


def rounded_diamond(size, radius, ring, color_ring, color_face):
    """The ONYX mark: a rounded square turned 45 degrees with a ring."""
    big = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((0, 0, size - 1, size - 1), radius, fill=color_ring + (255,))
    d.rounded_rectangle((ring, ring, size - 1 - ring, size - 1 - ring), max(1, radius - ring),
                        fill=color_face + (255,))
    return big.rotate(45, resample=Image.BICUBIC, expand=True)


def main():
    img = np.tile(BG, (H, W, 1))

    # Soft aurora behind the mark, a cooler wash behind the wordmark.
    lx, ly = 148 * S, 150 * S
    img += glow(lx, ly, 150 * S, 125 * S, VIOLET, 0.42)
    img += glow(lx - 30 * S, ly - 40 * S, 90 * S, 70 * S, CYAN, 0.10)
    img += glow(430 * S, 150 * S, 220 * S, 90 * S, (70, 60, 140), 0.18)

    # Faint dot grid that fades out with the glow.
    grid = dot_grid(14 * S, int(0.9 * S), 0.10)
    mask = np.exp(-(((np.mgrid[0:H, 0:W][1] - lx) / (260 * S)) ** 2 +
                    ((np.mgrid[0:H, 0:W][0] - ly) / (150 * S)) ** 2))[..., None]
    img += grid * mask

    # Blend all decoration to the exact background at the borders.
    img = BG + (img - BG) * edge_fade()
    base = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")

    # Mark: ring glow, ring, two screens.
    side = 150 * S
    mark = rounded_diamond(side, 34 * S, 9 * S, VIOLET, FACE)
    halo = rounded_diamond(side, 34 * S, 9 * S, VIOLET, VIOLET)
    halo_a = halo.split()[3].filter(ImageFilter.GaussianBlur(18 * S)).point(lambda v: int(v * 0.65))
    halo.putalpha(halo_a)
    mx, my = lx - mark.width // 2, ly - mark.height // 2
    base.alpha_composite(halo, (mx, my))
    base.alpha_composite(mark, (mx, my))

    d = ImageDraw.Draw(base)
    top = (lx - 40 * S, ly - 50 * S, lx + 40 * S, ly - 7 * S)
    bot = (lx - 30 * S, ly + 7 * S, lx + 30 * S, ly + 49 * S)
    # Glossy top screen: cyan with a subtle lighter upper band.
    d.rounded_rectangle(top, 7 * S, fill=CYAN + (255,))
    d.rounded_rectangle((top[0] + 3 * S, top[1] + 3 * S, top[2] - 3 * S, top[1] + 14 * S), 5 * S,
                        fill=(170, 236, 248, 255))
    d.rounded_rectangle(bot, 7 * S, fill=LAVENDER + (255,))
    d.rounded_rectangle((bot[0] + 3 * S, bot[1] + 3 * S, bot[2] - 3 * S, bot[1] + 12 * S), 5 * S,
                        fill=(220, 214, 255, 255))

    # Wordmark with a two-tone treatment and a thin accent line + tagline.
    f_big = ImageFont.truetype(FONT_BOLD, 62 * S)
    f_small = ImageFont.truetype(FONT_MED, 15 * S)
    tx = 284 * S
    onyx_w = d.textlength("ONYX ", font=f_big)
    asc, desc = f_big.getmetrics()
    ty = 150 * S - (asc + desc) // 2 - 12 * S
    # Shadow/glow under the text for depth.
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((tx, ty), "ONYX 3DS", font=f_big, fill=VIOLET + (150,))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10 * S))
    base.alpha_composite(shadow)
    d = ImageDraw.Draw(base)
    d.text((tx, ty), "ONYX", font=f_big, fill=(242, 242, 250, 255))
    d.text((tx + onyx_w, ty), "3DS", font=f_big, fill=LAVENDER + (255,))

    full_w = d.textlength("ONYX 3DS", font=f_big)
    ly2 = ty + asc + desc + 2 * S
    # Accent line: violet fading to cyan.
    for i in range(int(full_w)):
        t = i / max(1, full_w - 1)
        c = tuple(int(VIOLET[k] + (CYAN[k] - VIOLET[k]) * t) for k in range(3))
        a = int(255 * min(1.0, (1 - t) * 4) * min(1.0, t * 12 + 0.2))
        d.line((tx + i, ly2, tx + i, ly2 + int(2.2 * S)), fill=c + (a,))
    d.text((tx + 2 * S, ly2 + 10 * S), "3 D S   E M U L A T O R   F O R   X B O X",
           font=f_small, fill=(150, 152, 178, 255))

    out = base.convert("RGB")
    out.resize((1240, 600), Image.LANCZOS).save(os.path.join(ASSETS, "SplashScreen.scale-200.png"),
                                                 optimize=True)
    out.resize((620, 300), Image.LANCZOS).save(os.path.join(ASSETS, "SplashScreen.png"), optimize=True)


if __name__ == "__main__":
    main()

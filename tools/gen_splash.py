#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the OnyxEMU splash screen and the Start/home-menu tile logos.

The splash is a static PNG shown by the OS while the app starts, so it costs
nothing at boot. Its edges fade to exactly the manifest BackgroundColor
(#101218) so the image blends into the full-screen fill with no visible box.
Run from the repo root:  python3 tools/gen_splash.py
Output: app/ONYX3DS/Assets/SplashScreen.png (620x300) and
        SplashScreen.scale-200.png (1240x600, used on Xbox at 1080p/4K),
        Square150x150Logo, Square44x44Logo (+ unplated 24), StoreLogo, Wide310x150Logo
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


def gem(size):
    """The OnyxEMU mark: a brilliant-cut onyx gem, dark pavilion, violet crown, bright rim."""
    n = size * 4
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def P(x, y):
        return (x * n, y * n)

    tl, tr = P(0.28, 0.16), P(0.72, 0.16)          # table
    gl, gr = P(0.03, 0.40), P(0.97, 0.40)          # girdle ends
    a1, a2 = P(0.40, 0.40), P(0.60, 0.40)          # girdle points under the table
    m1, m2 = P(0.5, 0.16), P(0.5, 0.40)
    apex = P(0.5, 0.97)
    d.polygon([tl, tr, gr, apex, gl], fill=(22, 18, 40, 255))
    d.polygon([tl, tr, a2, a1], fill=(198, 184, 255, 255))                  # table
    d.polygon([tl, a1, gl], fill=(138, 104, 246, 255))                      # crown left
    d.polygon([tr, a2, gr], fill=(104, 76, 214, 255))                       # crown right
    d.polygon([gl, a1, apex], fill=(54, 40, 120, 255))                      # pavilion left
    d.polygon([a1, a2, apex], fill=(34, 26, 78, 255))                       # pavilion centre
    d.polygon([a2, gr, apex], fill=(70, 52, 150, 255))                      # pavilion right
    d.polygon([tl, m1, a1], fill=(150, 232, 246, 255))                      # glint
    w = max(2, int(n * 0.02))
    edge = (24, 18, 48, 255)
    for pts in ((gl, gr), (a1, tl), (a2, tr), (a1, apex), (a2, apex), (a1, a2)):
        d.line(list(pts), fill=edge, width=max(1, w // 2))
    d.line([tl, tr, gr, apex, gl, tl], fill=(240, 234, 255, 255), width=w, joint="curve")
    return img.resize((size, size), Image.LANCZOS)


def wordmark(d, x, y, font, big=True):
    onyx_w = d.textlength("Onyx", font=font)
    d.text((x, y), "Onyx", font=font, fill=(242, 242, 250, 255))
    d.text((x + onyx_w, y), "EMU", font=font, fill=LAVENDER + (255,))
    return onyx_w + d.textlength("EMU", font=font)


def tiles():
    def plate(w, h):
        im = Image.new("RGBA", (w, h), tuple(int(v) for v in BG) + (255,))
        return im
    # Square tiles: gem only (the OS prints the short name on the 150 tile).
    for name, size, frac, dy in (("Square150x150Logo.png", 150, 0.58, -0.08),
                                 ("Square44x44Logo.png", 44, 0.86, 0.0),
                                 ("StoreLogo.png", 50, 0.84, 0.0)):
        im = plate(size, size)
        g = gem(int(size * frac))
        im.alpha_composite(g, ((size - g.width) // 2, int((size - g.height) / 2 + size * dy)))
        im.convert("RGB").save(os.path.join(ASSETS, name), optimize=True)
    g = gem(22)
    im = Image.new("RGBA", (24, 24), (0, 0, 0, 0))
    im.alpha_composite(g, (1, 1))
    im.save(os.path.join(ASSETS, "Square44x44Logo.targetsize-24_altform-unplated.png"), optimize=True)
    # Wide tile: gem plus wordmark.
    k = 4
    im = Image.new("RGBA", (310 * k, 150 * k), tuple(int(v) for v in BG) + (255,))
    g = gem(96 * k)
    im.alpha_composite(g, (22 * k, 27 * k))
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(FONT_BOLD, 38 * k)
    asc, desc = f.getmetrics()
    wordmark(d, 128 * k, 75 * k - (asc + desc) // 2 - 4 * k, f)
    d.text((130 * k, 96 * k), "EMULATION FOR XBOX", font=ImageFont.truetype(FONT_MED, 10 * k),
           fill=(150, 152, 178, 255))
    im.convert("RGB").resize((310, 150), Image.LANCZOS).save(os.path.join(ASSETS, "Wide310x150Logo.png"),
                                                              optimize=True)


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

    # Mark: soft glow behind the gem.
    msize = 172 * S
    mark = gem(msize)
    pad = 40 * S
    halo_src = Image.new("L", (msize + 2 * pad, msize + 2 * pad), 0)
    halo_src.paste(mark.split()[3], (pad, pad))
    halo_a = halo_src.filter(ImageFilter.GaussianBlur(18 * S)).point(lambda v: int(v * 0.5))
    halo = Image.new("RGBA", halo_src.size, VIOLET + (0,))
    halo.putalpha(halo_a)
    mx, my = lx - msize // 2, ly - msize // 2
    base.alpha_composite(halo, (mx - pad, my - pad))
    base.alpha_composite(mark, (mx, my))
    d = ImageDraw.Draw(base)

    # Wordmark with a two-tone treatment and a thin accent line + tagline.
    f_big = ImageFont.truetype(FONT_BOLD, 58 * S)
    f_small = ImageFont.truetype(FONT_MED, 15 * S)
    tx = 268 * S
    asc, desc = f_big.getmetrics()
    ty = 150 * S - (asc + desc) // 2 - 12 * S
    # Shadow/glow under the text for depth.
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((tx, ty), "OnyxEMU", font=f_big, fill=VIOLET + (150,))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10 * S))
    base.alpha_composite(shadow)
    d = ImageDraw.Draw(base)
    full_w = wordmark(d, tx, ty, f_big)
    ly2 = ty + asc + desc + 2 * S
    # Accent line: violet fading to cyan.
    for i in range(int(full_w)):
        t = i / max(1, full_w - 1)
        c = tuple(int(VIOLET[k] + (CYAN[k] - VIOLET[k]) * t) for k in range(3))
        a = int(255 * min(1.0, (1 - t) * 4) * min(1.0, t * 12 + 0.2))
        d.line((tx + i, ly2, tx + i, ly2 + int(2.2 * S)), fill=c + (a,))
    d.text((tx + 2 * S, ly2 + 10 * S), "E M U L A T I O N   F O R   X B O X",
           font=f_small, fill=(150, 152, 178, 255))

    out = base.convert("RGB")
    out.resize((1240, 600), Image.LANCZOS).save(os.path.join(ASSETS, "SplashScreen.scale-200.png"),
                                                 optimize=True)
    out.resize((620, 300), Image.LANCZOS).save(os.path.join(ASSETS, "SplashScreen.png"), optimize=True)


def readme_logos():
    """Transparent logos for the README: dark text for light pages, light text for dark pages."""
    k = 2
    W2, H2 = 360 * k * 2, 163 * k * 2
    for name, ink, accent in (("logo-light.png", (24, 22, 40), (96, 70, 200)),
                              ("logo-dark.png", (240, 240, 248), LAVENDER)):
        im = Image.new("RGBA", (W2, H2), (0, 0, 0, 0))
        g = gem(int(H2 * 0.78))
        im.alpha_composite(g, (int(H2 * 0.06), int(H2 * 0.11)))
        d = ImageDraw.Draw(im)
        f = ImageFont.truetype(FONT_BOLD, int(H2 * 0.25))
        asc, desc = f.getmetrics()
        x, y = int(H2 * 0.98), (H2 - (asc + desc)) // 2 - int(H2 * 0.02)
        ow = d.textlength("Onyx", font=f)
        d.text((x, y), "Onyx", font=f, fill=ink + (255,))
        d.text((x + ow, y), "EMU", font=f, fill=accent + (255,))
        im.resize((W2 // 2, H2 // 2), Image.LANCZOS).save(os.path.join(HERE, "..", "docs", "assets", name),
                                                            optimize=True)


if __name__ == "__main__":
    main()
    tiles()
    readme_logos()

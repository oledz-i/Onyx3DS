#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Composites a home-menu preview from a theme's textures, laid out the same
way MainPage does (4x3 channel grid, bottom bar with clock and buttons).
Placeholder "game art" is generated so no real box art is used.
Usage: python3 tools/render_preview.py <theme-id> <out.png>"""
import json
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(__file__), "..", "app", "ONYX3DS", "Assets", "Themes")
W, H = 1920, 1080


def font(size, bold=False):
    bundled = os.path.join(os.path.dirname(__file__), "..", "app", "ONYX3DS", "Assets", "Fonts",
                           "RoundedMplus1c-Bold.ttf" if bold else "RoundedMplus1c-Medium.ttf")
    if os.path.exists(bundled):
        return ImageFont.truetype(bundled, size)
    for name in (["DejaVuSans-Bold.ttf"] if bold else ["DejaVuSans.ttf"]):
        for d in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def hex_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def fake_art(w, h, seed, title):
    """Abstract placeholder art: gradient sky, hills, a title card."""
    rnd = random.Random(seed)
    hue = rnd.random()
    import colorsys
    c1 = tuple(int(v * 255) for v in colorsys.hsv_to_rgb(hue, 0.55, 0.95))
    c2 = tuple(int(v * 255) for v in colorsys.hsv_to_rgb((hue + 0.12) % 1, 0.75, 0.55))
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=tuple(int(c1[i] * (1 - t) + c2[i] * t) for i in range(3)))
    for k in range(3):
        base = h * (0.55 + 0.13 * k)
        pts = [(x, base + math.sin(x / w * math.tau * (1 + k) + seed) * h * 0.06) for x in range(0, w + 8, 8)]
        shade = tuple(max(0, int(v * (0.75 - 0.15 * k))) for v in c2)
        d.polygon(pts + [(w, h), (0, h)], fill=shade)
    f = font(int(h * 0.15), True)
    tw = d.textlength(title, font=f)
    d.text(((w - tw) / 2 + 3, h * 0.18 + 3), title, font=f, fill=(0, 0, 0))
    d.text(((w - tw) / 2, h * 0.18), title, font=f, fill=(255, 255, 255))
    return img


def rounded(img, r):
    m = Image.new("L", (img.width * 4, img.height * 4), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, m.width - 1, m.height - 1], r * 4, fill=255)
    out = img.convert("RGBA")
    out.putalpha(m.resize(img.size, Image.LANCZOS))
    return out


def main(theme_id, out_path):
    folder = os.path.join(ROOT, theme_id)
    t = json.load(open(os.path.join(folder, "theme.json")))
    tex = {k: os.path.join(folder, v) for k, v in t["textures"].items()}
    col = t["colors"]
    style = t["style"]

    canvas = Image.open(tex["background"]).convert("RGBA").resize((W, H))
    far = Image.open(tex["background_far"]).convert("RGBA").resize((W, H), Image.BILINEAR)
    near = Image.open(tex["background_near"]).convert("RGBA").resize((W, H), Image.BILINEAR)
    canvas = Image.alpha_composite(canvas, far)
    canvas = Image.alpha_composite(canvas, near)

    frame = Image.open(tex["tile_frame"]).convert("RGBA")
    gloss = Image.open(tex["tile_gloss"]).convert("RGBA")
    empty = Image.open(tex["tile_empty"]).convert("RGBA")
    cursor = Image.open(tex["cursor"]).convert("RGBA")

    cols, rows = 4, 3
    tw, th = 384, 216
    gap_x, gap_y = 40, 40
    grid_w = cols * tw + (cols - 1) * gap_x
    x0, y0 = (W - grid_w) // 2, 92
    titles = ["Kart Racer", "Sky Quest", "Puzzle Pals", "Dragon Tales", "Pixel Farm", "Beat Dash",
              None, None, None, None, None, None]
    radius = int(style["tile_corner_radius"])
    focus = 1
    for i, title in enumerate(titles):
        cx, cy = x0 + (i % cols) * (tw + gap_x), y0 + (i // cols) * (th + gap_y)
        scale = style["hover_scale"] if i == focus else 1.0
        w, h = int(tw * scale), int(th * scale)
        px, py = cx - (w - tw) // 2, cy - (h - th) // 2
        # drop shadow (depth)
        depth = int(style["tile_depth"] * (1.6 if i == focus else 1.0))
        sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle([px + 10, py + depth + 6, px + w - 10, py + h + depth * 0.5],
                                             radius, fill=(0, 0, 0, 80 if title else 30))
        canvas = Image.alpha_composite(canvas, sh.filter(ImageFilter.GaussianBlur(depth * 0.8)))
        if title is None:
            canvas.alpha_composite(empty.resize((w, h), Image.LANCZOS), (px, py))
            continue
        art = rounded(fake_art(w - 20, h - 20, i * 7 + 3, title), max(radius - 6, 2))
        canvas.alpha_composite(art, (px + 10, py + 10))
        canvas.alpha_composite(frame.resize((w, h), Image.LANCZOS), (px, py))
        canvas.alpha_composite(gloss.resize((w, h), Image.LANCZOS), (px, py))
        if i == focus:
            cw, ch = int(w * 560 / 512), int(h * 336 / 288)
            canvas.alpha_composite(cursor.resize((cw, ch), Image.LANCZOS), (px - (cw - w) // 2, py - (ch - h) // 2))

    bar = Image.open(tex["bar"]).convert("RGBA")
    canvas.alpha_composite(bar, (0, H - bar.height))
    d = ImageDraw.Draw(canvas)
    text = hex_rgb(col["bar_text"])
    clock = "3:42"
    f_clock = font(64, True)
    cw = d.textlength(clock, font=f_clock)
    d.text(((W - cw) / 2, H - 150), clock, font=f_clock, fill=text)
    f_date = font(26)
    date = "Sat 10/3"
    dw = d.textlength(date, font=f_date)
    d.text(((W - dw) / 2, H - 76), date, font=f_date, fill=text)
    btn = Image.open(tex["button"]).convert("RGBA").resize((112, 112), Image.LANCZOS)
    for bx, label, glyph in ((96, "Settings", "\u2699"), (W - 208, "Library", "\u25A6")):
        canvas.alpha_composite(btn, (bx, H - 182))
        f_g = font(44, True)
        gw = d.textlength(glyph, font=f_g)
        d.text((bx + 52 - gw / 2, H - 160), glyph, font=f_g, fill=hex_rgb(col["accent"]))
        f_b = font(22, True)
        lw = d.textlength(label, font=f_b)
        d.text((bx + 56 - lw / 2, H - 62), label, font=f_b, fill=text)
    # Title strip over the focused channel
    f_t = font(30, True)
    d.text((x0, 30), "ONYX 3DS", font=f_t, fill=hex_rgb(col["text"]))
    f_s = font(24)
    hint = "Sky Quest  ·  Last played yesterday  ·  2h 14m"
    hw = d.textlength(hint, font=f_s)
    d.text((x0 + grid_w - hw, 34), hint, font=f_s, fill=hex_rgb(col["text_muted"]))
    canvas.convert("RGB").save(out_path, quality=92)
    print("wrote", out_path)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the built-in home-menu themes for ONYX 3DS.

Every texture is drawn procedurally (gradients, blurred light, grain, glass
highlights), so the themes are original artwork with no third-party assets.
Run from the repo root:  python3 tools/gen_theme_art.py
Output: app/ONYX3DS/Assets/Themes/<theme-id>/{theme.json, *.png, *.jpg}
"""
import json
import math
import os
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(__file__), "..", "app", "ONYX3DS", "Assets", "Themes")
W, H = 1920, 1080


def hex_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def lerp(a, b, t):
    return a + (b - a) * t


def vertical_gradient(top, bottom, w=W, h=H, curve=1.0):
    t = np.linspace(0, 1, h)[:, None] ** curve
    top, bottom = np.array(hex_rgb(top), float), np.array(hex_rgb(bottom), float)
    img = top[None, None, :] * (1 - t[..., None]) + bottom[None, None, :] * t[..., None]
    return np.repeat(img, w, axis=1)


def radial_light(img, cx, cy, radius, color, strength):
    h, w, _ = img.shape
    y, x = np.mgrid[0:h, 0:w]
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / radius
    a = np.clip(1 - d, 0, 1) ** 2 * strength
    c = np.array(hex_rgb(color), float)
    return img * (1 - a[..., None]) + c * a[..., None]


def light_rays(img, color, origin_x, count, strength, seed):
    """Soft diagonal beams falling from above the screen."""
    h, w, _ = img.shape
    layer = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(layer)
    rnd = random.Random(seed)
    for _ in range(count):
        x = origin_x + rnd.uniform(-w * 0.35, w * 0.35)
        spread = rnd.uniform(60, 180)
        angle = rnd.uniform(0.25, 0.45)
        top = [(x - spread * 0.3, -10), (x + spread * 0.3, -10)]
        bottom = [(x + h * angle + spread, h * 0.9), (x + h * angle - spread, h * 0.9)]
        d.polygon(top + bottom, fill=int(255 * rnd.uniform(0.35, 1.0)))
    layer = layer.filter(ImageFilter.GaussianBlur(60))
    a = np.asarray(layer, float)[..., None] / 255 * strength
    c = np.array(hex_rgb(color), float)
    return img * (1 - a) + c * a


def horizon_grid(img, color, horizon_y, alpha):
    """Retro perspective grid fading toward the horizon."""
    h, w, _ = img.shape
    layer = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(layer)
    vx = w / 2
    for i in range(-24, 25):
        d.line([(vx + i * 12, horizon_y), (vx + i * 260, h)], fill=255, width=2)
    y = horizon_y
    step = 6.0
    while y < h:
        d.line([(0, y), (w, y)], fill=255, width=2)
        step *= 1.18
        y += step
    yy = np.arange(h)[:, None]
    fade = np.clip((yy - horizon_y) / (h - horizon_y), 0, 1) ** 0.8
    a = np.asarray(layer.filter(ImageFilter.GaussianBlur(0.8)), float) / 255 * fade * alpha
    c = np.array(hex_rgb(color), float)
    glow = np.exp(-((yy - horizon_y) / 40.0) ** 2) * 0.5  # bright haze on the horizon line
    out = img * (1 - a[..., None]) + c * a[..., None]
    return out * (1 - glow[..., None]) + np.array(hex_rgb("#FFD8A8"), float) * glow[..., None]


def vignette(img, strength=0.25):
    h, w, _ = img.shape
    y, x = np.mgrid[0:h, 0:w]
    d = np.sqrt(((x - w / 2) / (w / 2)) ** 2 + ((y - h / 2) / (h / 2)) ** 2) / math.sqrt(2)
    return img * (1 - strength * d[..., None] ** 2)


def grain(img, amount, seed):
    rng = np.random.default_rng(seed)
    return img + rng.normal(0, amount, img.shape[:2])[..., None]


def pinstripes(img, period, contrast, angle_deg=0.0):
    h, w, _ = img.shape
    y, x = np.mgrid[0:h, 0:w]
    a = math.radians(angle_deg)
    coord = y * math.cos(a) + x * math.sin(a)
    s = (np.sin(coord / period * 2 * math.pi) * 0.5 + 0.5) ** 6
    return img * (1 - contrast * s[..., None]) + 255 * contrast * 0.5 * s[..., None]


def waves(img, color, base_y, amp, count, alpha, seed):
    """Soft flowing ribbons near the bottom, for depth behind the bar."""
    h, w, _ = img.shape
    layer = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(layer)
    rnd = random.Random(seed)
    for i in range(count):
        phase = rnd.random() * math.tau
        freq = rnd.uniform(0.6, 1.4)
        y0 = base_y + i * 26
        pts = [(x, y0 + math.sin(x / w * math.tau * freq + phase) * amp) for x in range(0, w + 20, 20)]
        d.line(pts, fill=int(255 * alpha * (1 - i / count)), width=10 + i * 3)
    layer = layer.filter(ImageFilter.GaussianBlur(14))
    a = np.asarray(layer, float)[..., None] / 255
    c = np.array(hex_rgb(color), float)
    return img * (1 - a) + c * a


def save_rgb(arr, path, quality=90):
    Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").save(path, quality=quality, optimize=True)


def bokeh_layer(colors, count, size_range, alpha_range, blur, seed, w=960, h=540, rings=False):
    """Transparent layer of out-of-focus light orbs (drawn at half res)."""
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for _ in range(count):
        r = rnd.uniform(*size_range)
        x, y = rnd.uniform(-r, w + r), rnd.uniform(-r, h + r)
        col = hex_rgb(rnd.choice(colors))
        a = int(255 * rnd.uniform(*alpha_range))
        orb = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(orb)
        d.ellipse([x - r, y - r, x + r, y + r], fill=col + (a,))
        if rings:  # glass bubble: brighter rim and a small specular dot
            d.ellipse([x - r, y - r, x + r, y + r], outline=(255, 255, 255, min(255, a + 60)), width=2)
            d.ellipse([x - r * 0.55, y - r * 0.65, x - r * 0.15, y - r * 0.3], fill=(255, 255, 255, min(255, a + 80)))
        orb = blur_rgba(orb, blur if not rings else blur * 0.35)
        img = Image.alpha_composite(img, orb)
    return img


def blur_rgba(img, radius):
    """Gaussian blur in premultiplied space so transparent pixels do not
    bleed black into soft edges (PIL blurs straight alpha otherwise)."""
    a = np.asarray(img, float)
    alpha = a[..., 3:4] / 255.0
    pre = np.concatenate([a[..., :3] * alpha, a[..., 3:4]], axis=2)
    chans = [Image.fromarray(np.clip(pre[..., i], 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius))
             for i in range(4)]
    pb = np.stack([np.asarray(c, float) for c in chans], axis=2)
    out_a = pb[..., 3:4]
    rgb = np.where(out_a > 0, pb[..., :3] * 255.0 / np.maximum(out_a, 1e-6), 0)
    return Image.fromarray(np.clip(np.concatenate([rgb, out_a], axis=2), 0, 255).astype(np.uint8), "RGBA")


def rounded_mask(w, h, r, scale=4):
    m = Image.new("L", (w * scale, h * scale), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w * scale - 1, h * scale - 1], r * scale, fill=255)
    return m.resize((w, h), Image.LANCZOS)


def channel_frame(face, edge, highlight, w=512, h=288, r=36, depth=1.0):
    """Glossy channel tile frame: bevelled rim, inner shadow, lit top edge.
    The centre is transparent so game art shows through (drawn 9-slice)."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    outer = rounded_mask(w, h, r)
    inset = 14
    inner = Image.new("L", (w, h), 0)
    inner.paste(rounded_mask(w - 2 * inset, h - 2 * inset, r - inset // 2), (inset, inset))
    rim = Image.fromarray(np.clip(np.asarray(outer, int) - np.asarray(inner, int), 0, 255).astype(np.uint8))
    # Rim colour: vertical metallic gradient.
    g = vertical_gradient(highlight, edge, w, h, 1.3)
    g = g * 0.55 + np.array(hex_rgb(face), float) * 0.45
    rim_rgb = Image.fromarray(np.clip(g, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    rim_rgb.putalpha(rim)
    img = Image.alpha_composite(img, rim_rgb)
    # Bevel: light from the top-left on the rim.
    bevel = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bevel)
    bd.rounded_rectangle([3, 3, w - 4, h - 4], r - 2, outline=(255, 255, 255, int(200 * depth)), width=3)
    bevel = blur_rgba(bevel, 1.5)
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shade).rounded_rectangle([inset - 2, inset - 2, w - inset + 1, h - inset + 1], r - inset // 2,
                                            outline=(0, 0, 0, int(70 * depth)), width=4)
    shade = shade.filter(ImageFilter.GaussianBlur(3))
    img = Image.alpha_composite(img, bevel)
    img = Image.alpha_composite(img, shade)
    # Keep everything inside the rounded outline.
    a = np.minimum(np.asarray(img.getchannel("A")), np.asarray(outer))
    img.putalpha(Image.fromarray(a.astype(np.uint8)))
    return img


def gloss(w=512, h=288, r=36, strength=0.55):
    """Curved glass highlight over the top of a tile."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    g = np.zeros((h, w), float)
    yy, xx = np.mgrid[0:h, 0:w]
    edge = h * 0.46 + np.sin(xx / w * math.pi) * h * 0.06
    g = np.clip((edge - yy) / edge, 0, 1) ** 1.6 * 255 * strength
    hl = Image.fromarray(g.astype(np.uint8))
    hl = Image.fromarray(np.minimum(np.asarray(hl), np.asarray(rounded_mask(w, h, r))).astype(np.uint8))
    img.paste((255, 255, 255, 255), (0, 0), hl)
    return img


def empty_slot(face, edge, w=512, h=288, r=36, alpha=0.55):
    """Frosted, slightly recessed empty channel."""
    m = rounded_mask(w, h, r)
    base = vertical_gradient(face, edge, w, h, 1.0)
    yy, xx = np.mgrid[0:h, 0:w]
    dots = ((xx % 18 < 2) & (yy % 18 < 2)).astype(float) * 18  # subtle dot grid
    base = base - dots[..., None]
    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    a = (np.asarray(m, float) * alpha).astype(np.uint8)
    img.putalpha(Image.fromarray(a))
    inner = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(inner).rounded_rectangle([4, 4, w - 5, h - 5], r - 3, outline=(0, 0, 0, 45), width=6)
    inner = inner.filter(ImageFilter.GaussianBlur(5))
    img = Image.alpha_composite(img, inner)
    a2 = np.minimum(np.asarray(img.getchannel("A")), np.asarray(m))
    img.putalpha(Image.fromarray(a2.astype(np.uint8)))
    return img


def cursor_glow(color, w=560, h=336, r=44, blur=14):
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = hex_rgb(color)
    d.rounded_rectangle([18, 18, w - 19, h - 19], r, outline=c + (255,), width=10)
    img = blur_rgba(img, blur)
    crisp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(crisp).rounded_rectangle([22, 22, w - 23, h - 23], r - 4, outline=c + (230,), width=4)
    return Image.alpha_composite(img, crisp)


def bottom_bar(top_col, bottom_col, line_col, w=W, h=230):
    """Bottom bar whose top edge is a gentle arch (higher in the middle), with
    fine horizontal lines, a bright rim and a soft shadow cast upwards."""
    base = vertical_gradient(top_col, bottom_col, w, h, 0.8)
    yy, xx = np.mgrid[0:h, 0:w]
    base = base - ((yy % 4) == 0)[..., None] * 6
    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB").convert("RGBA")

    def edge(x):
        t = x / w
        return 78 - 46 * math.sin(math.pi * t)

    ss = 3
    mask = Image.new("L", (w * ss, h * ss), 0)
    pts = [(x * ss, edge(x) * ss) for x in range(0, w + 1, 6)]
    ImageDraw.Draw(mask).polygon(pts + [(w * ss, h * ss), (0, h * ss)], fill=255)
    img.putalpha(mask.resize((w, h), Image.LANCZOS))

    curve = [(x, edge(x)) for x in range(0, w + 1, 6)]
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).line([(x, y - 8) for x, y in curve], fill=(0, 0, 0, 60), width=16)
    shadow = shadow.filter(ImageFilter.GaussianBlur(10))
    rim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(rim).line(curve, fill=hex_rgb(line_col) + (255,), width=4)
    hi = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(hi).line([(x, y + 5) for x, y in curve], fill=(255, 255, 255, 90), width=3)
    out = Image.alpha_composite(shadow, img)
    out = Image.alpha_composite(out, blur_rgba(rim, 0.6))
    return Image.alpha_composite(out, blur_rgba(hi, 1))


def round_button(face, rim, glyph_col, size=176):
    s = size * 4
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([8, 16, s - 8, s], fill=(0, 0, 0, 60))  # drop shadow
    d.ellipse([0, 0, s - 16, s - 16], fill=hex_rgb(rim) + (255,))
    d.ellipse([18, 18, s - 34, s - 34], fill=hex_rgb(face) + (255,))
    img = blur_rgba(img, 2).resize((size, size), Image.LANCZOS)
    # Inner gloss
    gl = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(gl).ellipse([size * 0.22, size * 0.10, size * 0.70, size * 0.36], fill=(255, 255, 255, 70))
    gl = blur_rgba(gl, 8)
    return Image.alpha_composite(img, gl)


THEMES = [
    {
        "id": "aero-channel",
        "name": "Aero Channel",
        "description": "Bright, airy and glassy, with soft light and floating bubbles. The default.",
        "colors": {
            "background_top": "#F8FBFD", "background_bottom": "#D5E3EE", "accent": "#2FB4E9",
            "accent_text": "#FFFFFF", "text": "#4B5866", "text_muted": "#8C99A6", "tile_face": "#FFFFFF",
            "tile_edge": "#AFC2D1", "tile_glow": "#5CCBF5", "bar": "#E8EEF3", "bar_text": "#5B6875",
            "panel": "#FFFFFFEE"},
        "bg": {"top": "#F4FAFE", "bottom": "#B9D7EA", "light": "#FFFFFF", "light2": "#8FDDF8",
               "stripes": 0.045, "wave": "#7FD0F2", "wave2": "#FFFFFF", "wave_alpha": 0.55,
               "rays": 0.45, "vignette": 0.14, "grain": 2.2},
        "bokeh": ["#BDEBFA", "#FFFFFF", "#D7F2FF", "#A9E2F7"],
        "bubbles": ["#E9F8FF", "#FFFFFF"],
        "bar": ("#F4F7FA", "#C9D6E0", "#9FB3C2"),
        "button": ("#FFFFFF", "#3CB9EC"),
        "style": {"tile_corner_radius": 18, "tile_depth": 12, "tile_tilt": 6, "parallax": 1.0,
                  "hover_scale": 1.08, "empty_slots": 12, "show_clock": True,
                  "font": "Segoe UI Variable Display"},
        "music": "aero-channel.m4a",
    },
    {
        "id": "midnight-onyx",
        "name": "Midnight Onyx",
        "description": "Polished black glass with violet and teal glow.",
        "colors": {
            "background_top": "#11121A", "background_bottom": "#05060A", "accent": "#9B7BFF",
            "accent_text": "#FFFFFF", "text": "#E7E8F2", "text_muted": "#8B8FA8", "tile_face": "#1A1C27",
            "tile_edge": "#3A3D52", "tile_glow": "#7C5CFF", "bar": "#14151E", "bar_text": "#C9CBE0",
            "panel": "#16171FEE"},
        "bg": {"top": "#1A1B27", "bottom": "#050508", "light": "#4B2E9E", "light2": "#0E6C78",
               "stripes": 0.04, "wave": "#5B3FD0", "wave2": "#1FB6C9", "wave_alpha": 0.40,
               "rays": 0.25, "vignette": 0.45, "grain": 3.0},
        "bokeh": ["#7C5CFF", "#22C3D6", "#B49CFF", "#3B2D8F"],
        "bubbles": ["#8E7BFF", "#49D6E6"],
        "bar": ("#232533", "#0B0C12", "#6D5AE0"),
        "button": ("#22243A", "#8F73FF"),
        "style": {"tile_corner_radius": 16, "tile_depth": 18, "tile_tilt": 7, "parallax": 1.0,
                  "hover_scale": 1.08, "empty_slots": 12, "show_clock": True,
                  "font": "Segoe UI Variable Display"},
        "music": "midnight-onyx.m4a",
    },
    {
        "id": "sunset-arcade",
        "name": "Sunset Arcade",
        "description": "Warm dusk gradient, neon haze and a retro horizon.",
        "colors": {
            "background_top": "#2A1748", "background_bottom": "#FF7E5F", "accent": "#FF4F9A",
            "accent_text": "#FFFFFF", "text": "#FFF3EA", "text_muted": "#F2C1B4", "tile_face": "#2B1838",
            "tile_edge": "#FF9F7A", "tile_glow": "#FF5FA8", "bar": "#24122F", "bar_text": "#FFD8C9",
            "panel": "#2A1636EE"},
        "bg": {"top": "#1E0F3A", "bottom": "#FF8A5B", "light": "#FFD27A", "light2": "#FF4F9A",
               "stripes": 0.05, "wave": "#FF6FB0", "wave_alpha": 0.25, "rays": 0.25,
               "grid": "#FF5FBF", "vignette": 0.30, "grain": 2.8},
        "bokeh": ["#FF8FB8", "#FFC27A", "#FF5F8A", "#C47BFF"],
        "bubbles": ["#FFD1A8", "#FF9FC8"],
        "bar": ("#3A1C4A", "#170A22", "#FF6FAE"),
        "button": ("#3A1C4A", "#FF6FAE"),
        "style": {"tile_corner_radius": 14, "tile_depth": 16, "tile_tilt": 8, "parallax": 1.2,
                  "hover_scale": 1.09, "empty_slots": 8, "show_clock": True,
                  "font": "Bahnschrift"},
        "music": "sunset-arcade.m4a",
    },
    {
        "id": "matcha",
        "name": "Matcha",
        "description": "Calm green paper tones with soft daylight.",
        "colors": {
            "background_top": "#F3F6EC", "background_bottom": "#CFDDBF", "accent": "#5C9E4F",
            "accent_text": "#FFFFFF", "text": "#3F4A38", "text_muted": "#7D8A72", "tile_face": "#FBFCF7",
            "tile_edge": "#AEBF9E", "tile_glow": "#86C46F", "bar": "#E6EDDB", "bar_text": "#4F5C46",
            "panel": "#FBFCF7EE"},
        "bg": {"top": "#F7F9F1", "bottom": "#C9D9B6", "light": "#FFFFF2", "light2": "#DDEFC2",
               "stripes": 0.0, "wave": "#A9CC8F", "wave2": "#FFFFFF", "wave_alpha": 0.45,
               "rays": 0.35, "vignette": 0.12, "grain": 4.0},
        "bokeh": ["#E9F5D4", "#FFFFFF", "#CFE7B5"],
        "bubbles": ["#F4FAEA", "#FFFFFF"],
        "bar": ("#F1F5EA", "#C8D6B6", "#8FAE79"),
        "button": ("#FBFCF7", "#6FAE5E"),
        "style": {"tile_corner_radius": 20, "tile_depth": 10, "tile_tilt": 4, "parallax": 0.8,
                  "hover_scale": 1.06, "empty_slots": 12, "show_clock": True,
                  "font": "Segoe UI Variable Display"},
        "music": "aero-channel.m4a",
    },
    {
        "id": "carbon",
        "name": "Carbon",
        "description": "No channels nostalgia: a clean dark grid with sharp corners.",
        "colors": {
            "background_top": "#1C1E21", "background_bottom": "#0F1012", "accent": "#3DDC97",
            "accent_text": "#0B0C0E", "text": "#ECEFF2", "text_muted": "#8E959C", "tile_face": "#22252A",
            "tile_edge": "#3A3F46", "tile_glow": "#3DDC97", "bar": "#16181B", "bar_text": "#C9CED4",
            "panel": "#1A1C1FEE"},
        "bg": {"top": "#202328", "bottom": "#0C0D0F", "light": "#2B3B35", "light2": "#1E2A33",
               "stripes": 0.06, "wave": "#2A5C49", "vignette": 0.40, "grain": 3.5},
        "bokeh": ["#2E6B54", "#37474F"],
        "bubbles": ["#3DDC97"],
        "bar": ("#1F2226", "#0E0F11", "#3DDC97"),
        "button": ("#22252A", "#3DDC97"),
        "style": {"tile_corner_radius": 6, "tile_depth": 8, "tile_tilt": 0, "parallax": 0.4,
                  "hover_scale": 1.05, "empty_slots": 0, "show_clock": True,
                  "font": "Segoe UI Variable Display"},
        "music": "midnight-onyx.m4a",
    },
]


def build(theme, seed):
    out = os.path.join(ROOT, theme["id"])
    os.makedirs(out, exist_ok=True)
    c = theme["colors"]
    bg = theme["bg"]

    img = vertical_gradient(bg["top"], bg["bottom"], curve=1.2)
    img = radial_light(img, W * 0.22, H * 0.05, W * 0.75, bg["light"], 0.55)
    img = radial_light(img, W * 0.92, H * 0.95, W * 0.6, bg["light2"], 0.35)
    if bg["stripes"]:
        img = pinstripes(img, 5.0, bg["stripes"])
    img = light_rays(img, bg["light"], W * 0.3, 7, bg.get("rays", 0.35), seed + 5)
    img = waves(img, bg["wave"], H * 0.62, 42, 7, bg.get("wave_alpha", 0.45), seed)
    if "wave2" in bg:
        img = waves(img, bg["wave2"], H * 0.70, 30, 5, bg.get("wave_alpha", 0.45) * 0.8, seed + 9)
    if "grid" in bg:
        img = horizon_grid(img, bg["grid"], H * 0.58, 0.55)
    img = vignette(img, bg["vignette"])
    img = grain(img, bg["grain"], seed)
    save_rgb(img, os.path.join(out, "background.jpg"))

    bokeh_layer(theme["bokeh"], 34, (18, 70), (0.10, 0.35), 18, seed + 1).save(
        os.path.join(out, "background_far.png"), optimize=True)
    bokeh_layer(theme["bubbles"], 14, (6, 22), (0.18, 0.40), 6, seed + 2, rings=True).save(
        os.path.join(out, "background_near.png"), optimize=True)

    r = int(theme["style"]["tile_corner_radius"] * 2)
    channel_frame(c["tile_face"], c["tile_edge"], "#FFFFFF" if c["tile_face"] > "#888888" else c["tile_edge"],
                  r=r).save(os.path.join(out, "tile_frame.png"), optimize=True)
    gloss(r=r, strength=0.55 if c["tile_face"] > "#888888" else 0.28).save(
        os.path.join(out, "tile_gloss.png"), optimize=True)
    empty_slot(c["tile_face"], c["tile_edge"], r=r).save(os.path.join(out, "tile_empty.png"), optimize=True)
    cursor_glow(c["tile_glow"], r=r + 8).save(os.path.join(out, "cursor.png"), optimize=True)
    bottom_bar(*theme["bar"]).save(os.path.join(out, "bar.png"), optimize=True)
    round_button(*theme["button"], glyph_col=c["text"]).save(os.path.join(out, "button.png"), optimize=True)

    manifest = {
        "id": theme["id"], "name": theme["name"], "author": "ONYX 3DS",
        "description": theme["description"], "colors": c,
        "textures": {"background": "background.jpg", "background_far": "background_far.png",
                     "background_near": "background_near.png", "tile_frame": "tile_frame.png",
                     "tile_gloss": "tile_gloss.png", "tile_empty": "tile_empty.png", "bar": "bar.png",
                     "button": "button.png", "cursor": "cursor.png"},
        "audio": {"music": "../../Audio/" + theme["music"], "move": "../../Audio/move.wav",
                  "select": "../../Audio/select.wav", "back": "../../Audio/back.wav",
                  "launch": "../../Audio/launch.wav"},
        "style": theme["style"],
    }
    with open(os.path.join(out, "theme.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("built", theme["id"])


if __name__ == "__main__":
    for i, t in enumerate(THEMES):
        build(t, 1000 + i * 17)

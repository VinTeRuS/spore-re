#!/usr/bin/env python3
"""Draws the launcher icon of the Android port (original artwork, no game assets or logos: a soft spiral galaxy in
deep space with a single-celled organism in front of it, the journey from cell to space) and writes
android/res/mipmap-*/ic_launcher.png, the adaptive icon layers and android/icon-512.png.

Usage: tools/make_icon.py"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
TEAL = (64, 214, 200)
VIOLET = (150, 92, 230)
CELL = (120, 236, 150)


def background(n):
    """Deep space with stars and a two-armed spiral galaxy."""
    img = Image.new("RGB", (n, n), (8, 10, 24))
    rnd = random.Random(1948)
    stars = ImageDraw.Draw(img)
    for _ in range(n // 3):
        x, y = rnd.uniform(0, n), rnd.uniform(0, n)
        r = rnd.choice((1, 1, 1, 2)) * n / 1024
        v = rnd.randint(120, 255)
        stars.ellipse((x - r, y - r, x + r, y + r), fill=(v, v, min(255, v + 20)))
    glow = Image.new("RGB", (n, n), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cx = cy = n / 2
    for arm in range(2):
        for i in range(2400):
            t = i / 2400 * 3.4 * math.pi
            r = n * 0.04 + t * n * 0.034
            a = t + arm * math.pi
            x = cx + r * math.cos(a) + rnd.gauss(0, n * 0.012)
            y = cy + r * math.sin(a) * 0.82 + rnd.gauss(0, n * 0.012)
            mix = min(1.0, r / (n * 0.42))
            col = tuple(int(TEAL[k] * (1 - mix) + VIOLET[k] * mix) for k in range(3))
            s = n * 0.006 * (1.4 - mix)
            gd.ellipse((x - s, y - s, x + s, y + s), fill=col)
    core = n * 0.09
    gd.ellipse((cx - core, cy - core, cx + core, cy + core), fill=(240, 236, 220))
    glow = ImageEnhance.Brightness(glow.filter(ImageFilter.GaussianBlur(n * 0.010))).enhance(2.6)
    sharp = ImageEnhance.Brightness(glow.filter(ImageFilter.GaussianBlur(n * 0.003))).enhance(1.2)
    lit = Image.composite(glow, img, glow.convert("L"))
    return Image.composite(sharp, lit, sharp.convert("L").point(lambda v: min(255, v * 2)))


def organism(n):
    """A round cell with a translucent membrane, a nucleus, a few organelles and cilia (RGBA, transparent around)."""
    layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cx, cy, r = n * 0.5, n * 0.54, n * 0.2
    for i in range(14):  # cilia
        a = i / 14 * 2 * math.pi
        x0, y0 = cx + r * math.cos(a), cy + r * math.sin(a)
        bend = 0.35 * math.sin(i * 1.7)
        x1, y1 = cx + r * 1.32 * math.cos(a + bend * 0.2), cy + r * 1.32 * math.sin(a + bend * 0.2)
        d.line((x0, y0, x1, y1), fill=CELL + (200,), width=max(1, int(n * 0.012)))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=CELL + (90,), outline=CELL + (255,), width=max(1, int(n * 0.016)))
    nr = r * 0.38  # nucleus
    d.ellipse((cx - nr * 0.6, cy - nr * 0.9, cx + nr * 1.4, cy + nr * 1.1), fill=(40, 120, 90, 230),
              outline=(200, 255, 220, 255), width=max(1, int(n * 0.008)))
    for ox, oy, s in ((-0.5, -0.35, 0.12), (-0.45, 0.4, 0.09), (0.55, -0.45, 0.08)):  # organelles
        x, y, q = cx + ox * r, cy + oy * r, s * r
        d.ellipse((x - q, y - q, x + q, y + q), fill=(220, 255, 230, 200))
    halo = layer.filter(ImageFilter.GaussianBlur(n * 0.02))
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.alpha_composite(halo)
    out.alpha_composite(layer)
    return out


def draw(n=1024, layers=False):
    bg = background(n)
    fg = organism(n)
    if layers:
        return bg, fg
    img = bg.convert("RGBA")
    img.alpha_composite(fg)
    return img.convert("RGB")


def main():
    big = draw()
    for name, px in SIZES.items():
        d = os.path.join(ROOT, "android", "res", f"mipmap-{name}")
        os.makedirs(d, exist_ok=True)
        big.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
    big.resize((512, 512), Image.LANCZOS).save(os.path.join(ROOT, "android", "icon-512.png"))
    # Adaptive icon (API 26+): 108 dp layers, artwork inside the 66 dp safe zone.
    bg, fg = draw(layers=True)
    n = fg.size[0]
    inner = int(n * 66 / 108)
    safe = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    safe.alpha_composite(fg.resize((inner, inner), Image.LANCZOS), ((n - inner) // 2, (n - inner) // 2))
    for name, px in SIZES.items():
        d = os.path.join(ROOT, "android", "res", f"mipmap-{name}")
        layer_px = px * 108 // 48
        bg.resize((layer_px, layer_px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_bg.png"))
        safe.resize((layer_px, layer_px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_fg.png"))


if __name__ == "__main__":
    main()

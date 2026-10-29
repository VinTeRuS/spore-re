#!/usr/bin/env python3
"""Draws the launcher icon of the Android port: original artwork (no game assets, logos or wordmarks), a vivid
cartoon spiral galaxy with a warm glowing core, saturated cyan-to-magenta arms and sparkling stars on a deep
blue-violet sky. Writes android/res/mipmap-*/ic_launcher.png, the adaptive icon layers and android/icon-512.png.

Usage: tools/make_icon.py"""
import math
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
SKY_TOP = (34, 20, 92)
SKY_BOTTOM = (6, 26, 74)
ARM_INNER = (90, 230, 255)
ARM_MID = (120, 120, 255)
ARM_OUTER = (240, 90, 220)
CORE = (255, 236, 170)


def lerp(a, b, t):
    return tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3))


def sky(n):
    img = Image.new("RGB", (n, n))
    d = ImageDraw.Draw(img)
    for y in range(n):
        d.line((0, y, n, y), fill=lerp(SKY_TOP, SKY_BOTTOM, y / (n - 1)))
    return img


def sparkle(d, x, y, r, col):
    """A four-pointed cartoon star."""
    w = r * 0.22
    d.polygon([(x, y - r), (x + w, y - w), (x + r, y), (x + w, y + w), (x, y + r), (x - w, y + w), (x - r, y),
               (x - w, y - w)], fill=col)


def galaxy(n):
    """The galaxy on a transparent layer (additive glow, then crisp highlights)."""
    rnd = random.Random(2008)
    glow = Image.new("RGB", (n, n), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cx, cy = n * 0.5, n * 0.5
    tilt = math.radians(-20)
    arms = 3
    for arm in range(arms):
        for i in range(1800):
            t = i / 1800
            ang = t * 2.6 * math.pi + arm * 2 * math.pi / arms
            r = n * (0.05 + 0.36 * t)
            spread = n * 0.022 * (0.3 + t)
            x = r * math.cos(ang) + rnd.gauss(0, spread * 0.5)
            y = r * math.sin(ang) * 0.78 + rnd.gauss(0, spread * 0.5)
            x, y = cx + x * math.cos(tilt) - y * math.sin(tilt), cy + x * math.sin(tilt) + y * math.cos(tilt)
            col = lerp(ARM_INNER, ARM_MID, t * 2) if t < 0.5 else lerp(ARM_MID, ARM_OUTER, (t - 0.5) * 2)
            s = n * (0.012 - 0.007 * t)
            gd.ellipse((x - s, y - s, x + s, y + s), fill=tuple(int(c * 0.62) for c in col))
    for k, (rr, a) in enumerate(((0.10, 0.45), (0.065, 0.75), (0.04, 1.0))):  # core
        r = n * rr
        gd.ellipse((cx - r, cy - r * 0.86, cx + r, cy + r * 0.86), fill=tuple(int(c * a) for c in CORE))
    soft = glow.filter(ImageFilter.GaussianBlur(n * 0.025))
    mid = glow.filter(ImageFilter.GaussianBlur(n * 0.005))
    light = ImageChops.add(soft, mid)
    light = ImageChops.add(light, mid.filter(ImageFilter.GaussianBlur(n * 0.002)))
    # bright arm highlights: thin crisp strokes along each arm
    hl = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    for arm in range(arms):
        pts = []
        for i in range(200):
            t = i / 200
            ang = t * 2.6 * math.pi + arm * 2 * math.pi / arms
            r = n * (0.05 + 0.34 * t)
            x, y = r * math.cos(ang), r * math.sin(ang) * 0.78
            pts.append((cx + x * math.cos(tilt) - y * math.sin(tilt), cy + x * math.sin(tilt) + y * math.cos(tilt)))
        for i in range(len(pts) - 1):
            t = i / len(pts)
            if t < 0.25:  # the core is bright enough on its own
                continue
            hd.line((pts[i], pts[i + 1]), fill=(200, 250, 255, int(220 * (1 - t))),
                    width=max(1, int(n * 0.007 * (1 - t) + 1)))
    hl = hl.filter(ImageFilter.GaussianBlur(n * 0.003))
    layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    # Additive light as an alpha layer: colour normalized to full brightness, alpha from its brightness.
    alpha = light.convert("L").point(lambda v: min(255, int(v * 1.3)))
    layer.paste(light, (0, 0), alpha)
    layer.alpha_composite(hl)
    core = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    r = n * 0.035
    ImageDraw.Draw(core).ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 250, 225, 255))
    layer.alpha_composite(core.filter(ImageFilter.GaussianBlur(n * 0.012)))
    return layer


def stars(n, layer):
    rnd = random.Random(7)
    d = ImageDraw.Draw(layer)
    for _ in range(90):
        x, y = rnd.uniform(0, n), rnd.uniform(0, n)
        r = rnd.choice((1, 1, 2)) * n / 900
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, rnd.randint(120, 255)))
    for x, y, r, col in ((0.18, 0.2, 0.035, (255, 255, 255, 230)), (0.84, 0.14, 0.028, (255, 230, 150, 230)),
                         (0.86, 0.78, 0.03, (170, 240, 255, 230)), (0.12, 0.82, 0.022, (255, 180, 240, 220))):
        sparkle(d, x * n, y * n, r * n, col)


def draw(n=1024, layers=False):
    bg = sky(n).convert("RGBA")
    stars(n, bg)
    fg = galaxy(n)
    if layers:
        return bg.convert("RGB"), fg
    img = bg.copy()
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
    inner = int(n * 72 / 108)
    safe = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    safe.alpha_composite(fg.resize((inner, inner), Image.LANCZOS), ((n - inner) // 2, (n - inner) // 2))
    for name, px in SIZES.items():
        d = os.path.join(ROOT, "android", "res", f"mipmap-{name}")
        layer_px = px * 108 // 48
        bg.resize((layer_px, layer_px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_bg.png"))
        safe.resize((layer_px, layer_px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_fg.png"))


if __name__ == "__main__":
    main()

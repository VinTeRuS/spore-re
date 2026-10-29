#!/usr/bin/env python3
"""Draws the launcher icon of the Android port: original artwork in a bright, playful cartoon style (no game assets,
logos or characters from the game): a round glossy critter with eyes on stalks, a smile and spots, on a sky-blue to
teal backdrop. Writes android/res/mipmap-*/ic_launcher.png, the adaptive icon layers and android/icon-512.png.

Usage: tools/make_icon.py"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
BODY = (255, 150, 40)        # warm orange critter
BODY_DARK = (214, 92, 20)
SPOT = (255, 210, 90)
SKY_TOP = (90, 210, 255)
SKY_BOTTOM = (20, 120, 170)


def background(n):
    """Vertical sky-blue gradient with a soft light from the upper left and a few bubbles."""
    img = Image.new("RGB", (n, n))
    px = img.load()
    for y in range(n):
        t = y / (n - 1)
        row = tuple(int(SKY_TOP[k] * (1 - t) + SKY_BOTTOM[k] * t) for k in range(3))
        for x in range(n):
            px[x, y] = row
    light = Image.new("L", (n, n), 0)
    ImageDraw.Draw(light).ellipse((-n * 0.3, -n * 0.4, n * 0.7, n * 0.5), fill=110)
    light = light.filter(ImageFilter.GaussianBlur(n * 0.12))
    img = Image.composite(Image.new("RGB", (n, n), (230, 250, 255)), img, light)
    d = ImageDraw.Draw(img, "RGBA")
    for bx, by, br in ((0.14, 0.78, 0.05), (0.86, 0.24, 0.035), (0.82, 0.84, 0.025), (0.2, 0.2, 0.02)):
        x, y, r = bx * n, by * n, br * n
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, 60), outline=(255, 255, 255, 150),
                  width=max(1, int(n * 0.004)))
    return img


def eye(d, x, y, r, look):
    d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255), outline=(40, 30, 60), width=max(1, int(r * 0.12)))
    pr = r * 0.48
    px, py = x + look[0] * r * 0.32, y + look[1] * r * 0.32
    d.ellipse((px - pr, py - pr, px + pr, py + pr), fill=(30, 24, 50))
    hr = pr * 0.38
    d.ellipse((px - pr * 0.35 - hr, py - pr * 0.4 - hr, px - pr * 0.35 + hr, py - pr * 0.4 + hr), fill=(255, 255, 255))


def critter(n):
    """The character on a transparent layer: body, belly, spots, stalk eyes, smile, little feet, gloss, shadow."""
    layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cx, cy, rx, ry = n * 0.5, n * 0.6, n * 0.27, n * 0.23
    w = max(1, int(n * 0.012))
    # stalks and eyes behind the head line
    for side, tilt in ((-1, -0.25), (1, 0.2)):
        bx, by = cx + side * rx * 0.42, cy - ry * 0.8
        ex, ey = bx + side * n * 0.07, by - n * 0.2
        d.line((bx, by, (bx + ex) / 2 + side * n * 0.01, (by + ey) / 2, ex, ey), fill=BODY_DARK, width=int(n * 0.03),
               joint="curve")
        eye(d, ex, ey, n * 0.075, (0.4 * side + 0.2, 0.2 + tilt * 0.3))
    # feet
    for fx in (-0.5, 0.5):
        x, y = cx + fx * rx, cy + ry * 0.92
        d.ellipse((x - n * 0.06, y - n * 0.035, x + n * 0.06, y + n * 0.04), fill=BODY_DARK)
    # body and belly
    d.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=BODY, outline=BODY_DARK, width=w)
    d.ellipse((cx - rx * 0.6, cy - ry * 0.05, cx + rx * 0.6, cy + ry * 0.85), fill=(255, 205, 140))
    for sx, sy, sr in ((-0.62, -0.35, 0.11), (0.6, -0.45, 0.09), (0.75, 0.1, 0.07), (-0.8, 0.15, 0.06)):
        x, y, r = cx + sx * rx, cy + sy * ry, sr * rx * 2
        d.ellipse((x - r, y - r, x + r, y + r), fill=SPOT)
    # smile
    mw = rx * 0.55
    d.arc((cx - mw, cy - ry * 0.35, cx + mw, cy + ry * 0.45), start=20, end=160, fill=(90, 30, 20),
          width=int(n * 0.018))
    # gloss on the upper left of the body
    gloss = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(gloss).ellipse((cx - rx * 0.75, cy - ry * 0.85, cx - rx * 0.1, cy - ry * 0.35),
                                  fill=(255, 255, 255, 120))
    gloss = gloss.filter(ImageFilter.GaussianBlur(n * 0.01))
    layer.alpha_composite(gloss)
    # soft ground shadow under everything
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse((cx - rx * 1.05, cy + ry * 0.95, cx + rx * 1.05, cy + ry * 1.25),
                                   fill=(0, 30, 50, 110))
    out.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(n * 0.02)))
    out.alpha_composite(layer)
    return out


def draw(n=1024, layers=False):
    bg = background(n)
    fg = critter(n)
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

#!/usr/bin/env python3
"""Original HOME Menu banner for the 3DS port: a 256x128 image (night sky, a generic blue ocarina, plain text)
and a short generic chime (two sine notes, not a melody from the game). Writes banner.png and banner.wav;
tools/make_banner.sh turns them into port/banner.bnr with bannertool.

usage: make_banner.py OUT_DIR [FIGURE.png]   FIGURE (transparent background) replaces the ocarina:
tools/make_link_icon.py passes Link's render for the local-only port/banner_local.bnr."""
import math, struct, sys, wave
from PIL import Image, ImageDraw, ImageFont

out = sys.argv[1] if len(sys.argv) > 1 else "."
figure = Image.open(sys.argv[2]).convert("RGBA") if len(sys.argv) > 2 else None
W, H, S = 256, 128, 4  # drawn at 4x, downscaled
img = Image.new("RGBA", (W * S, H * S))
d = ImageDraw.Draw(img)
for y in range(H * S):
    t = y / (H * S - 1)
    d.line([(0, y), (W * S, y)], fill=(int(14 + 26 * t), int(22 + 46 * t), int(60 + 64 * t), 255))
for (x, y, r) in ((60, 70, 3), (190, 40, 2), (300, 90, 3), (420, 30, 2), (600, 60, 3), (760, 35, 2), (900, 80, 3)):
    d.ellipse([x - r * S // 2, y - r * S // 2, x + r * S // 2, y + r * S // 2], fill=(230, 235, 255, 255))
body = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
if figure is None:  # ocarina (as tools/make_icon.py, larger)
    b = ImageDraw.Draw(body)
    ox, oy = 40, 150
    b.ellipse([ox, oy, ox + 260, oy + 150], fill=(40, 110, 220, 255), outline=(170, 210, 255, 255), width=8)
    b.rounded_rectangle([ox + 235, oy + 50, ox + 320, oy + 100], radius=16, fill=(200, 205, 215, 255),
                        outline=(240, 240, 250, 255), width=6)
    for hx, hy in ((ox + 70, oy + 55), (ox + 115, oy + 42), (ox + 160, oy + 48), (ox + 92, oy + 100),
                   (ox + 142, oy + 104)):
        b.ellipse([hx - 14, hy - 14, hx + 14, hy + 14], fill=(10, 30, 80, 255))
    b.ellipse([ox + 30, oy + 22, ox + 150, oy + 62], fill=(120, 180, 255, 110))
    body = body.rotate(18, resample=Image.BICUBIC, center=(ox + 160, oy + 75))
else:  # the figure, as tall as the banner allows, centered in the same left column
    figure = figure.crop(figure.getbbox())
    fh = (H - 8) * S
    fw = figure.width * fh // figure.height
    if fw > 100 * S:  # wider than the column: fit the width instead
        fw, fh = 100 * S, figure.height * 100 * S // figure.width
    body.alpha_composite(figure.resize((fw, fh), Image.LANCZOS), ((106 * S - fw) // 2, (H * S - fh) // 2))
img.alpha_composite(body)
d = ImageDraw.Draw(img)
x0, x1 = 112 * S, (W - 6) * S  # text column


def fit(text, size):
    """the largest font size (<= size) whose rendering of `text` fits the text column"""
    while size > 6:
        try:
            font = ImageFont.load_default(size=size * S)
        except TypeError:  # Pillow < 10.1: fixed bitmap font
            return ImageFont.load_default()
        if d.textbbox((0, 0), text, font=font)[2] <= x1 - x0:
            return font
        size -= 1
    return ImageFont.load_default()


big = fit("Ocarina of Time", 24)
d.text((x0, 30 * S), "Ocarina of Time", font=big, fill=(255, 236, 160, 255))
d.text((x0, 58 * S), "64", font=big, fill=(255, 236, 160, 255))
d.text((x0, 96 * S), "Unofficial native", font=fit("Unofficial native", 12), fill=(200, 220, 255, 255))
d.text((x0, 110 * S), "3DS port", font=fit("Unofficial native", 12), fill=(200, 220, 255, 255))
img.resize((W, H), Image.LANCZOS).convert("RGB").save(out + "/banner.png")

# chime: two soft sine notes (E5, B5) with a decay, 32 kHz mono 16-bit
rate = 32000
n = int(rate * 1.2)
samples = []
for i in range(n):
    t = i / rate
    v = 0.0
    for start, f in ((0.0, 659.25), (0.18, 987.77)):
        if t >= start:
            u = t - start
            v += 0.28 * math.sin(2 * math.pi * f * u) * math.exp(-4.0 * u)
    samples.append(int(max(-1.0, min(1.0, v)) * 32767))
with wave.open(out + "/banner.wav", "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(struct.pack("<%dh" % n, *samples))

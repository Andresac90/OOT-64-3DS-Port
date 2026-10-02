# Original 48x48 HOME Menu icon: a glowing fairy (white core, blue-cyan glow, four wings) on a night-sky
# gradient. Drawn at 4x and downscaled. usage: make_icon.py OUT.png  (also writes OUT_preview.png at 4x)
from PIL import Image, ImageDraw, ImageFilter, ImageChops
import math, sys

N = 192  # drawn at 4x, downscaled
C = (N / 2, N / 2 + 6)  # the fairy's centre

img = Image.new("RGBA", (N, N))
d = ImageDraw.Draw(img)
for y in range(N):  # night sky gradient
    t = y / (N - 1)
    d.line([(0, y), (N, y)], fill=(int(14 + 26 * t), int(22 + 46 * t), int(60 + 64 * t), 255))
for (x, y, r) in ((28, 30, 3), (160, 26, 2), (150, 160, 3), (34, 150, 2), (172, 98, 2), (18, 92, 2)):
    d.ellipse([x - r, y - r, x + r, y + r], fill=(220, 230, 255, 255))


def radial(radius, rgb, alpha, falloff):
    """a round glow: alpha * (1 - r/radius)^falloff"""
    layer = Image.new("RGBA", (N, N), rgb + (0,))
    a = Image.new("L", (N, N), 0)
    px = a.load()
    for y in range(N):
        for x in range(N):
            r = math.hypot(x - C[0], y - C[1]) / radius
            if r < 1.0:
                px[x, y] = int(255 * alpha * (1.0 - r) ** falloff)
    layer.putalpha(a)
    return layer


def wing(angle, length, width, lift):
    """one wing: a long translucent ellipse from the centre outwards, rotated by `angle` degrees"""
    w = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    wd = ImageDraw.Draw(w)
    x0 = C[0] + 6
    box = [x0, C[1] - width / 2 - lift, x0 + length, C[1] + width / 2 - lift]
    wd.ellipse(box, fill=(200, 235, 255, 120), outline=(235, 250, 255, 230), width=3)
    wd.line([(x0 + 6, C[1] - lift), (x0 + length - 10, C[1] - lift)], fill=(235, 250, 255, 110), width=2)
    return w.rotate(angle, resample=Image.BICUBIC, center=C)


img.alpha_composite(radial(96, (40, 120, 255), 0.7, 1.5))  # wide blue halo
for angle, length, width, lift in ((40, 54, 24, 0), (140, 54, 24, 0), (8, 40, 17, 0), (172, 40, 17, 0)):
    img.alpha_composite(wing(angle, length, width, lift))
img.alpha_composite(radial(60, (120, 210, 255), 1.0, 1.1))  # cyan glow
img.alpha_composite(radial(30, (255, 255, 255), 1.0, 0.6))  # white-hot core
d = ImageDraw.Draw(img)
for (x, y, s) in ((C[0] + 46, C[1] - 52, 7), (C[0] - 52, C[1] + 40, 5), (C[0] + 30, C[1] + 56, 4)):  # sparkles
    d.polygon([(x, y - s), (x + s / 3, y - s / 3), (x + s, y), (x + s / 3, y + s / 3), (x, y + s),
               (x - s / 3, y + s / 3), (x - s, y), (x - s / 3, y - s / 3)], fill=(235, 248, 255, 255))

mask = Image.new("L", (N, N), 0)  # rounded corners
ImageDraw.Draw(mask).rounded_rectangle([0, 0, N - 1, N - 1], radius=36, fill=255)
img.putalpha(ImageChops.multiply(img.getchannel("A"), mask))
out = img.resize((48, 48), Image.LANCZOS)
out.save(sys.argv[1])
out.resize((192, 192), Image.NEAREST).save(sys.argv[1].replace(".png", "_preview.png"))

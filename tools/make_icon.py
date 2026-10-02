# Original 48x48 HOME Menu icon: a generic blue ocarina with music notes on a night-sky gradient.
from PIL import Image, ImageDraw, ImageFilter
import math, sys
N = 192  # draw at 4x, downscale
img = Image.new("RGBA", (N, N))
d = ImageDraw.Draw(img)
for y in range(N):  # night sky gradient
    t = y / (N - 1)
    c = (int(18 + 30 * t), int(28 + 50 * t), int(70 + 60 * t), 255)
    d.line([(0, y), (N, y)], fill=c)
# rounded-corner mask
mask = Image.new("L", (N, N), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, N - 1, N - 1], radius=36, fill=255)
# ocarina body: an ellipse rotated ~-25 degrees, with a mouthpiece
body = Image.new("RGBA", (N, N), (0, 0, 0, 0))
b = ImageDraw.Draw(body)
b.ellipse([30, 70, 150, 140], fill=(40, 110, 220, 255), outline=(170, 210, 255, 255), width=5)
b.rounded_rectangle([138, 92, 178, 116], radius=8, fill=(200, 205, 215, 255), outline=(240, 240, 250, 255), width=3)
for hx, hy in ((62, 96), (84, 90), (106, 92), (74, 116), (98, 118)):  # finger holes
    b.ellipse([hx - 7, hy - 7, hx + 7, hy + 7], fill=(10, 30, 80, 255))
b.ellipse([44, 80, 100, 100], fill=(120, 180, 255, 110))  # highlight
body = body.rotate(22, resample=Image.BICUBIC, center=(N / 2, N / 2 + 10))
img.alpha_composite(body)
# music notes
d = ImageDraw.Draw(img)
def note(x, y, s, col):
    d.ellipse([x, y, x + 16 * s, y + 12 * s], fill=col)
    d.rectangle([x + 13 * s, y - 34 * s, x + 16 * s, y + 6 * s], fill=col)
    d.polygon([(x + 16 * s, y - 34 * s), (x + 30 * s, y - 24 * s), (x + 16 * s, y - 22 * s)], fill=col)
note(118, 48, 1.0, (255, 225, 110, 255))
note(150, 30, 0.8, (255, 225, 110, 255))
note(34, 40, 0.7, (200, 230, 255, 255))
out = Image.new("RGBA", (N, N), (0, 0, 0, 0))
out.paste(img, (0, 0))
out = out.resize((48, 48), Image.LANCZOS)
out.save(sys.argv[1])
out.resize((192, 192), Image.NEAREST).save(sys.argv[1].replace(".png", "_preview.png"))

#!/usr/bin/env python3
"""color2png.py - turn 3DS color dumps (sd_color0_<i>.bin from statedump.c) into 400x240 PNGs of the
WHOLE top screen, side bars included (fbdiff.py only compares the centered 320x240 N64 area).

The dump is the render target as read back by gfx_3ds.c: a PORTRAIT buffer (header: width, height =
240*sy, 400*sx) of 0xRRGGBBAA words, pixel (x, y) of the 400x240 screen at [x*sx+ox][W-1-(y*sy+oy)],
box-filtered over the sx*sy supersamples.

usage: color2png.py DUMP.bin [OUT.png]   or   color2png.py --dir SDDIR/tour OUTDIR
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fbdiff import write_png  # noqa: E402


def convert(src, dst):
    data = open(src, "rb").read()
    W, H = struct.unpack("<II", data[:8])
    px = struct.unpack("<%dI" % (W * H), data[8:8 + W * H * 4])
    sx, sy = H // 400, W // 240
    n = sx * sy
    rows = []
    for y in range(240):
        row = []
        for x in range(400):
            r = g = b = 0
            for ox in range(sx):
                base = (x * sx + ox) * W + W - 1 - y * sy
                for oy in range(sy):
                    w = px[base - oy]
                    r += (w >> 24) & 0xFF
                    g += (w >> 16) & 0xFF
                    b += (w >> 8) & 0xFF
            row.append((r // n, g // n, b // n))
        rows.append(row)
    write_png(dst, 400, 240, rows)


def main():
    if sys.argv[1] == "--dir":
        src_dir, out_dir = sys.argv[2], sys.argv[3]
        os.makedirs(out_dir, exist_ok=True)
        for f in sorted(os.listdir(src_dir)):
            if f.startswith("sd_color0_") and f.endswith(".bin"):
                convert(os.path.join(src_dir, f), os.path.join(out_dir, f.replace(".bin", ".png")))
    else:
        convert(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(sys.argv[1])[0] + ".png")


if __name__ == "__main__":
    main()

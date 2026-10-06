#!/usr/bin/env python3
"""make_showcase.py - README screenshots (both screens) taken from the running port in Azahar.

For each shot: builds the port booting straight into an entrance (GAME_EXTRA=-DPORT_START_ENTRANCE, the debug save),
runs it in Azahar with the top screen's shown buffer (settings flipdump=1) and the bottom screen (sdmc:/3ds/oot/
capture_bottom) dumped, and composes both screens in a console-style frame. The normal build is restored at the end.

The images show the game: they are made from YOUR copy, like the build itself.

usage: make_showcase.py [--secs 45] [--only NAME,...]
       writes docs/images/<name>.png (the shots below) - the README shows them.
Needs: an extracted ROM (as for any build), Azahar, Python 3 + Pillow.
"""
import argparse, os, shutil, struct, subprocess, sys, time

from PIL import Image, ImageDraw

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/oot")
OUT = os.path.join(REPO, "docs", "images")

# name, entrance, age (1 child, 0 adult)
SHOTS = [
    ("showcase", 0x0EE, 1),       # Kokiri Forest
    ("hyrule_field", 0x0CD, 1),   # Hyrule Field
    ("kakariko", 0x0DB, 1),       # Kakariko Village
    ("deku_tree", 0x000, 1),      # Inside the Deku Tree
]


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=REPO, **kw)


def build(entrance, age):
    # (noon: the debug save starts at midnight)
    extra = ("-DPORT_START_ENTRANCE=0x%03X -DPORT_START_AGE=%d -DPORT_START_DAYTIME=0x8000" % (entrance, age)
             if entrance is not None else "")
    sh("touch src/overlays/gamestates/ovl_opening/z_opening.c")
    r = sh("make -f Makefile.3ds -j8 cci GAME_EXTRA='%s' > /dev/null" % extra)
    if r.returncode != 0:
        sys.exit("build failed")


def top_image(path):
    """the flip buffer on screen: 400x240 (mode 0), 800x240 (mode 1, the wide mode) or two 400x240 eyes (mode 2),
    BGR8, stored as the panel scans it (columns of 240 pixels from the bottom)"""
    data = open(path, "rb").read()
    _, mode, nbytes = struct.unpack("<III", data[:12])
    px = data[12:12 + nbytes]
    width = 800 if mode == 1 else 400
    im = Image.frombytes("RGB", (240, width), px[:240 * width * 3], "raw", "BGR")
    im = im.transpose(Image.Transpose.ROTATE_90)
    return im.resize((800, 480), Image.LANCZOS) if width == 800 else im.resize((800, 480), Image.NEAREST)


def bottom_image(path):
    data = open(path, "rb").read()
    w, h, fmt = struct.unpack("<III", data[:12])
    px = data[12:]
    if fmt == 2:  # RGB565 (GSP_RGB565_OES)
        im = Image.frombytes("RGB", (w, h), px[:w * h * 2], "raw", "BGR;16")
    else:
        bpp = len(px) // (w * h)
        im = Image.frombytes("RGB", (w, h), px[:w * h * bpp], "raw", "BGR" if bpp == 3 else "BGRX")
    im = im.transpose(Image.Transpose.ROTATE_90)
    return im.resize((640, 480), Image.NEAREST)


def compose(top, bottom):
    """both screens in a console-like frame, as the first showcase"""
    W, pad, bez = 912, 24, 30
    th, bh = top.size[1], bottom.size[1]
    H = pad + th + 2 * bez + 16 + bh + 2 * bez + pad
    img = Image.new("RGB", (W, H), (28, 42, 34))
    d = ImageDraw.Draw(img)
    y0 = pad
    d.rounded_rectangle([pad, y0, W - pad, y0 + th + 2 * bez], radius=26, fill=(58, 61, 66), outline=(80, 84, 90), width=3)
    img.paste(top, ((W - top.size[0]) // 2, y0 + bez))
    y1 = y0 + th + 2 * bez + 16
    d.rounded_rectangle([pad, y1, W - pad, y1 + bh + 2 * bez], radius=26, fill=(58, 61, 66), outline=(80, 84, 90), width=3)
    img.paste(bottom, ((W - bottom.size[0]) // 2, y1 + bez))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=45)
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    want = set(args.only.split(",")) if args.only else None
    settings = os.path.join(SD, "settings.txt")
    keep = open(settings).read() if os.path.exists(settings) else "widescreen=0\n"
    os.makedirs(OUT, exist_ok=True)
    try:
        for name, entrance, age in SHOTS:
            if want and name not in want:
                continue
            print("== %s (entrance 0x%03X)" % (name, entrance), flush=True)
            build(entrance, age)
            for f in ("flip_shown.bin", "bottom_fb.bin"):
                if os.path.exists(os.path.join(SD, f)):
                    os.remove(os.path.join(SD, f))
            open(settings, "w").write("widescreen=1\nhud=1\nprof=1\nflipdump=1\n")
            open(os.path.join(SD, "capture_bottom"), "w").close()
            sh("tools/emu.sh boot %d > /dev/null 2>&1" % args.secs)
            os.remove(os.path.join(SD, "capture_bottom"))
            tp, bp = os.path.join(SD, "flip_shown.bin"), os.path.join(SD, "bottom_fb.bin")
            if not (os.path.exists(tp) and os.path.exists(bp)):
                print("   no dump (flip_shown.bin / bottom_fb.bin missing)")
                continue
            out = os.path.join(OUT, name + ".png")
            compose(top_image(tp), bottom_image(bp)).save(out, optimize=True)
            print("   wrote", out)
    finally:
        open(settings, "w").write(keep)
        build(None, 1)


if __name__ == "__main__":
    main()

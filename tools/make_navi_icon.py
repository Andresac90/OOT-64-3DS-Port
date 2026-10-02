#!/usr/bin/env python3
"""make_navi_icon.py - HOME Menu icon with Navi, rendered by the port from YOUR game data.

Navi's model is Nintendo's, so the image cannot be committed: this tool renders it locally and writes
port/icon_local.png (ignored by git), which Makefile.3ds uses instead of the original artwork port/icon.png.

How: builds the port with GAME_EXTRA=-DPORT_NAVIGEN (boots into Link's house; z_play.c draws Navi alone with her
own draw function, close up, over a black and then a white screen, and writes both frames in full color to
sdmc:/3ds/oot/navi_{black,white}.bin), runs it in Azahar and rebuilds the normal ROM. The two backgrounds give
the glow's transparency; Navi is then placed on the original icon's night sky.

usage: make_navi_icon.py [--raw]   (--raw: reuse the last navi_*.bin, only redo the image)
Needs: an extracted ROM (as for any build), Azahar, Python 3 + Pillow.
"""
import os, struct, sys, time

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_link_banner as L  # noqa: E402  (build, sh, background, paths)

TURNS = 4  # z_play.c PortNaviGen: 0, 45, 90, 135 degrees
FILES = tuple("navi_%d_%s.bin" % (t, c) for t in range(TURNS) for c in ("black", "white"))


def render():
    for f in FILES:
        if os.path.exists(os.path.join(L.SD, f)):
            os.remove(os.path.join(L.SD, f))
    L.build("-DPORT_NAVIGEN -DPORT_START_ENTRANCE=%s -DPORT_START_AGE=1" % L.ENTRANCE)
    try:
        L.sh("pkill -9 -f MacOS/azahar")
        time.sleep(1)
        app = L.sh("ls -d /Applications/*[Aa]zahar*/Azahar.app | head -1", capture_output=True, text=True).stdout.strip()
        if not app:
            sys.exit("Azahar not found in /Applications")
        L.sh("open -n '%s' --args '%s'" % (app, os.path.join(L.REPO, "build/3ds/oot.3ds")))
        deadline = time.time() + 180
        while not all(os.path.exists(os.path.join(L.SD, f)) for f in FILES):
            if time.time() > deadline:
                sys.exit("the Navi build never wrote %s (see boot.log there)" % " / ".join(FILES))
            time.sleep(2)
        time.sleep(2)
    finally:
        L.sh("pkill -9 -f MacOS/azahar")
        L.build("")  # leave the normal ROM in build/3ds


def decode(path):
    """the render target's readback (portrait, supersampled) as a landscape RGB image at full resolution"""
    data = open(path, "rb").read()
    W, H = struct.unpack("<II", data[:8])  # W = 240 * sy, H = 400 * sx
    px = struct.unpack("<%dI" % (W * H), data[8:8 + W * H * 4])
    img = Image.new("RGB", (H, W))
    out = img.load()
    for X in range(H):
        row = X * W
        for Y in range(W):
            v = px[row + W - 1 - Y]
            out[X, Y] = ((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF)
    return img


def matte(black, white):
    """color and alpha from the same render over black and over white, inside the N64's 320x240 picture (the
    side bars are black in both: they would read as opaque)"""
    w, h = black.size
    x0, x1 = w * 40 // 400, w * 360 // 400
    out = Image.new("RGBA", (w, h))
    bp, wp, op = black.load(), white.load(), out.load()
    for y in range(h):
        for x in range(x0, x1):
            b, t = bp[x, y], wp[x, y]
            a = 1.0 - sum(t[i] - b[i] for i in range(3)) / (3 * 255.0)
            a = 0.0 if a < 0.0 else 1.0 if a > 1.0 else a
            if a < 1.0 / 255:
                op[x, y] = (0, 0, 0, 0)
            else:
                op[x, y] = tuple(min(255, int(b[i] / a + 0.5)) for i in range(3)) + (int(a * 255 + 0.5),)
    return out


def compose(navi):
    """Navi on the night sky: her glow at the center, the square as large as her wings need"""
    a = navi.getchannel("A")
    w, h = navi.size
    px = a.load()
    sx = sy = n = 0
    for y in range(h):
        for x in range(w):
            if px[x, y] > 240:  # the glow's opaque core
                sx += x
                sy += y
                n += 1
    x0, y0, x1, y1 = a.point(lambda v: 255 if v > 24 else 0).getbbox()
    cx, cy = (sx // n, sy // n) if n else ((x0 + x1) // 2, (y0 + y1) // 2)
    half = int(max(cx - x0, x1 - cx, cy - y0, y1 - cy) * 1.05)
    crop = navi.crop((cx - half, cy - half, cx + half, cy + half))
    size = 192
    bg = L.background(size)
    bg.alpha_composite(crop.resize((size - 8, size - 8), Image.LANCZOS), (4, 4))
    return bg.resize((48, 48), Image.LANCZOS)


def wing_area(navi):
    """how much of her is wing (translucent, not glow): the turn that shows the wings spread has the most"""
    a = navi.getchannel("A")
    return sum(1 for v in a.getdata() if 24 < v < 200)


def main():
    if "--raw" not in sys.argv:
        render()
    turns = []
    for t in range(TURNS):
        navi = matte(decode(os.path.join(L.SD, FILES[2 * t])), decode(os.path.join(L.SD, FILES[2 * t + 1])))
        turns.append((wing_area(navi), t, navi))
        navi.save(os.path.join(L.REPO, "port/icon_local_turn%d.png" % t))
    area, t, navi = max(turns, key=lambda x: x[0])
    print("turn %d (%d degrees) shows the most wing" % (t, t * 45))
    if navi.getchannel("A").getbbox() is None:
        sys.exit("Navi is not in the frames (see navi_black.bin)")
    out = os.path.join(L.REPO, "port/icon_local.png")
    icon = compose(navi)
    icon.save(out)
    icon.resize((192, 192), Image.NEAREST).save(os.path.join(L.REPO, "port/icon_local_preview.png"))
    navi.save(os.path.join(L.REPO, "port/icon_local_source.png"))
    print("wrote %s (local only: Navi's model is the game's, never commit it)" % out)
    if "--raw" not in sys.argv:
        L.sh("make -f Makefile.3ds cci >/dev/null")
        print("built the normal ROM with it: build/3ds/oot.3ds")


if __name__ == "__main__":
    main()

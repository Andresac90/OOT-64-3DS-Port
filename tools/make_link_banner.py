#!/usr/bin/env python3
"""make_link_banner.py - HOME Menu banner (and optionally icon) with Link's 3D model, rendered by the port from
YOUR game data.

Link's model is Nintendo's, so the images cannot be committed: this tool renders them locally and writes
port/banner_local.bnr (and with --icon port/icon_local.png), both ignored by git. Makefile.3ds uses them when
they exist, else the original artwork port/banner.bnr and port/icon.png (tools/make_banner.sh, tools/make_icon.py).
The banner needs bannertool (https://github.com/diasurgical/bannertool) on PATH, in $BANNERTOOL or in
$DEVKITPRO/tools/bin.

How: builds the port with GAME_EXTRA=-DPORT_ICONGEN (boots straight into Link's house, opens the pause
menu, writes the Equipment page's Link preview, drawn at 2x = 128x224, to sdmc:/3ds/oot/link_icon.bin),
runs it in Azahar, then rebuilds the normal ROM. The preview is cut out of its black background; the banner is
tools/make_banner.py's with Link in place of the ocarina, the icon Link's head and shoulders on the original
icon's night sky.

usage: make_link_banner.py [--age adult|child] [--icon] [--raw link_icon.bin]   (--raw: only redo the images)
Needs: an extracted ROM (as for any build), Azahar, Python 3 + Pillow.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

from PIL import Image, ImageDraw

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/oot")
W, H = 128, 224  # PAUSE_EQUIP_PLAYER_WIDTH/HEIGHT at 2x (z_player_lib.c PORT_ICONGEN)
ENTRANCE = "0xBB"  # ENTR_LINKS_HOUSE_0: small scene, loads fast


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=REPO, **kw)


def build(extra):
    # make doesn't track flags: drop the objects of every game file that reacts to the tool defines
    hooked = sh("grep -rlE 'PORT_ICONGEN|PORT_NAVIGEN|PORT_START_ENTRANCE|PORT_START_AGE' src", capture_output=True,
                text=True).stdout.split()
    for src in hooked:
        o = os.path.join(REPO, "build/3ds", os.path.splitext(src)[0] + ".o")
        if os.path.exists(o):
            os.remove(o)
    r = sh("make -f Makefile.3ds cci GAME_EXTRA='%s' 2>&1 | grep -iE ' error|undefined reference' ; true" % extra,
           capture_output=True, text=True)
    if r.stdout.strip():
        sys.exit("3DS build failed:\n" + r.stdout)


def render(age):
    out = os.path.join(SD, "link_icon.bin")
    if os.path.exists(out):
        os.remove(out)
    build("-DPORT_ICONGEN -DPORT_START_ENTRANCE=%s -DPORT_START_AGE=%d" % (ENTRANCE, 0 if age == "adult" else 1))
    try:
        sh("pkill -9 -f MacOS/azahar")
        time.sleep(1)
        app = sh("ls -d /Applications/*[Aa]zahar*/Azahar.app | head -1", capture_output=True,
                 text=True).stdout.strip()
        if not app:
            sys.exit("Azahar not found in /Applications")
        sh("open -n '%s' --args '%s'" % (app, os.path.join(REPO, "build/3ds/oot.3ds")))
        deadline = time.time() + 180
        while not (os.path.exists(out) and os.path.getsize(out) == W * H * 2):
            if time.time() > deadline:
                sys.exit("the icon build never wrote %s (see boot.log there)" % out)
            time.sleep(2)
        time.sleep(1)
    finally:
        sh("pkill -9 -f MacOS/azahar")
        build("")  # leave the normal ROM in build/3ds
    return open(out, "rb").read()


def decode(raw):
    """The renderer's read-back layout (gfx_3ds.c read_back_offscreen): RGBA5551, pixel k at u16 index k^3."""
    img = Image.new("RGBA", (W, H))
    px = img.load()
    for k in range(W * H):
        i = (k ^ 3) * 2
        v = raw[i] | (raw[i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31
        px[k % W, k // W] = (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)
    # the preview is cleared to black: everything black that touches the border is background
    stack = [(x, y) for x in range(W) for y in (0, H - 1)] + [(x, y) for y in range(H) for x in (0, W - 1)]
    while stack:
        x, y = stack.pop()
        if 0 <= x < W and 0 <= y < H and px[x, y] == (0, 0, 0, 255):
            px[x, y] = (0, 0, 0, 0)
            stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return img


def background(n):
    """The original icon's night-sky gradient with rounded corners (tools/make_icon.py)."""
    img = Image.new("RGBA", (n, n))
    d = ImageDraw.Draw(img)
    for y in range(n):
        t = y / (n - 1)
        d.line([(0, y), (n, y)], fill=(int(18 + 30 * t), int(28 + 50 * t), int(70 + 60 * t), 255))
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, n - 1, n - 1], radius=n * 36 // 192, fill=255)
    img.putalpha(mask)
    return img


def compose(link):
    """Head and upper body: the top square of the figure's bounding box, on the background."""
    x0, y0, x1, y1 = link.getbbox()
    side = x1 - x0 + 8
    cx = (x0 + x1) // 2
    crop = link.crop((cx - side // 2, y0 - 4, cx - side // 2 + side, y0 - 4 + side))
    bg = background(192).resize((48, 48), Image.LANCZOS)  # smooth rounded corners
    bg.alpha_composite(crop.resize((44, 44), Image.LANCZOS), (2, 3))
    return bg


def bannertool():
    for path in (os.environ.get("BANNERTOOL"), shutil.which("bannertool"),
                 os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "tools/bin/bannertool")):
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def make_banner(link, out):
    """tools/make_banner.py's banner with Link in the left column, packed by bannertool. False: no bannertool."""
    tool = bannertool()
    if tool is None:
        return False
    with tempfile.TemporaryDirectory() as tmp:
        link.save(os.path.join(tmp, "link.png"))
        subprocess.run([sys.executable, os.path.join(REPO, "tools/make_banner.py"), tmp, os.path.join(tmp, "link.png")],
                       check=True)
        shutil.copy(os.path.join(tmp, "banner.png"), os.path.splitext(out)[0] + "_preview.png")
        subprocess.run([tool, "makebanner", "-i", os.path.join(tmp, "banner.png"), "-a",
                        os.path.join(tmp, "banner.wav"), "-o", out], check=True, stdout=subprocess.DEVNULL)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--age", choices=("adult", "child"), default="adult")
    ap.add_argument("--raw", help="reuse a link_icon.bin instead of running the game")
    ap.add_argument("--icon", action="store_true", help="also make the HOME Menu icon (default: the original one)")
    ap.add_argument("--icon-out", default=os.path.join(REPO, "port/icon_local.png"))
    ap.add_argument("--banner-out", default=os.path.join(REPO, "port/banner_local.bnr"))
    args = ap.parse_args()
    if bannertool() is None and not args.icon:
        sys.exit("bannertool not found (PATH, $BANNERTOOL or $DEVKITPRO/tools/bin): nothing to make")
    raw = open(args.raw, "rb").read() if args.raw else render(args.age)
    link = decode(raw)
    if link.getbbox() is None:
        sys.exit("the preview is empty")
    link.save(os.path.splitext(args.banner_out)[0] + "_source.png")
    if make_banner(link, args.banner_out):
        print("wrote %s" % args.banner_out)
    else:
        print("no bannertool found: banner not made (the build keeps port/banner.bnr)")
    if args.icon:
        icon = compose(link)
        icon.save(args.icon_out)
        icon.resize((192, 192), Image.NEAREST).save(os.path.splitext(args.icon_out)[0] + "_preview.png")
        print("wrote %s" % args.icon_out)
    print("local only: Link's model is the game's, never commit these files")
    if not args.raw:
        sh("make -f Makefile.3ds cci >/dev/null")  # the new files are newer than the icon and ROM: repacks only
        print("built the normal ROM with them: build/3ds/oot.3ds")


if __name__ == "__main__":
    main()

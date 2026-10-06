#!/usr/bin/env python3
"""make_link_banner.py - HOME Menu banner (and optionally icon) with Link's 3D model, captured by the port from
YOUR game data.

Link's model is Nintendo's, so these files cannot be committed: this tool makes them locally - port/banner_local.bnr,
port/banner_local_mesh.bin (and with --icon port/icon_local.png), all ignored by git. Makefile.3ds uses them when they
exist, else the original artwork port/banner.bnr and port/icon.png (tools/make_banner.sh, tools/make_icon.py).

How: builds the port with GAME_EXTRA=PORT_EXTRA=-DPORT_ICONGEN (boots straight into Link's house and opens the pause
menu). The Equipment page draws Link's preview (at 2x, 128x224) to link_icon.bin, and the renderer records the
triangles it draws there to link_mesh.bin. The banner is the picture on a transparent background (--white: on white)
in bannertool's standard picture banner, the format the real HOME Menu shows fine. --stereo makes the experimental
stereoscopic banner instead (tools/make_banner3d.py: his 3D model in front of the screen, or with --picture the
picture) - it still crashes the real HOME Menu (2026-10-06). The icon is Link's head and shoulders from the picture
on the original icon's night sky.

usage: make_link_banner.py [--age adult|child] [--icon] [--white] [--stereo [--picture]] [--raw link_icon.bin [--mesh link_mesh.bin]]
       (--raw: only redo the files from earlier captures)
Needs: an extracted ROM (as for any build), Azahar, Python 3.10+ with Pillow, bannertool
(https://github.com/diasurgical/bannertool) on PATH, in $BANNERTOOL or in $DEVKITPRO/tools/bin; for --stereo also
numpy, gltflib and pycgfx (https://github.com/skyfloogle/pycgfx) in tools/pycgfx/ or $PYCGFX.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

from PIL import Image, ImageDraw

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/oot")
W, H = 128, 224  # PAUSE_EQUIP_PLAYER_WIDTH/HEIGHT at 2x (z_player_lib.c PORT_ICONGEN)
ENTRANCE = "0xBB"  # ENTR_LINKS_HOUSE_0: small scene, loads fast


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=REPO, **kw)


def build(extra, port_extra=""):
    # make doesn't track flags: drop the objects of every file that reacts to the tool defines
    hooked = sh("grep -rlE 'PORT_ICONGEN|PORT_NAVIGEN|PORT_START_ENTRANCE|PORT_START_AGE' src port/src",
                capture_output=True, text=True).stdout.split()
    for src in hooked:
        o = os.path.join(REPO, "build/3ds", os.path.splitext(src)[0] + ".o")
        if os.path.exists(o):
            os.remove(o)
    r = sh("make -f Makefile.3ds cci GAME_EXTRA='%s' PORT_EXTRA='%s' 2>&1 | grep -iE ' error|undefined reference' ; true"
           % (extra, port_extra), capture_output=True, text=True)
    if r.stdout.strip():
        sys.exit("3DS build failed:\n" + r.stdout)


def render(age):
    """runs the capture build: the preview picture (link_icon.bin) and the preview's triangles (link_mesh.bin)"""
    out, mesh = os.path.join(SD, "link_icon.bin"), os.path.join(SD, "link_mesh.bin")
    for f in (out, mesh):
        if os.path.exists(f):
            os.remove(f)
    build("-DPORT_ICONGEN -DPORT_START_ENTRANCE=%s -DPORT_START_AGE=%d" % (ENTRANCE, 0 if age == "adult" else 1),
          "-DPORT_ICONGEN")
    try:
        sh("pkill -9 -f MacOS/azahar")
        time.sleep(1)
        app = sh("ls -d /Applications/*[Aa]zahar*/Azahar.app | head -1", capture_output=True,
                 text=True).stdout.strip()
        if not app:
            sys.exit("Azahar not found in /Applications")
        sh("open -n '%s' --args '%s'" % (app, os.path.join(REPO, "build/3ds/oot.3ds")))
        deadline = time.time() + 180
        while not (os.path.exists(out) and os.path.getsize(out) == W * H * 2 and os.path.exists(mesh)):
            if time.time() > deadline:
                sys.exit("the icon build never wrote %s / %s (see boot.log there)" % (out, mesh))
            time.sleep(2)
        time.sleep(2)
        shutil.copy(mesh, os.path.join(REPO, "port", "banner_local_mesh.bin"))
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


def make_banner(link, out, mesh=None, stereo=False, white=False):
    """the HOME Menu banner: Link's preview picture on a transparent background (white=True: on white), as a flat
    picture banner (bannertool's standard
    template, shown fine on hardware). stereo=True: the experimental stereoscopic CGFX banner (tools/make_banner3d.py,
    his 3D model when mesh is given) - it still crashes the real HOME Menu (2026-10-06), so it is not the default.
    False: no bannertool."""
    tool = bannertool()
    if tool is None:
        return False
    with tempfile.TemporaryDirectory() as tmp:
        if stereo:
            link.save(os.path.join(tmp, "link.png"))
            cmd = [sys.executable, os.path.join(REPO, "tools/make_banner3d.py"), out, "--figure",
                   os.path.join(tmp, "link.png"), "--preview", os.path.splitext(out)[0] + "_preview.png"]
            if mesh is not None:
                cmd += ["--model", mesh]
            subprocess.run(cmd, check=True, env=dict(os.environ, BANNERTOOL=tool))
            return True
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import make_banner3d  # (its banner chime)
        fig = link.crop(link.getbbox())
        scale = min(504 / fig.width, 244 / fig.height)  # composed at 512x256, 6 px margin, shown at 256x128
        fig = fig.resize((int(fig.width * scale), int(fig.height * scale)), Image.LANCZOS)
        pic = Image.new("RGBA", (512, 256), (255, 255, 255, 255 if white else 0))  # (the template's texture is RGBA4)
        pic.alpha_composite(fig, ((512 - fig.width) // 2, (256 - fig.height) // 2))
        pic = pic.resize((256, 128), Image.LANCZOS)
        png, wav = os.path.join(tmp, "banner.png"), os.path.join(tmp, "banner.wav")
        pic.save(png)
        pic.save(os.path.splitext(out)[0] + "_preview.png")
        make_banner3d.chime(wav)
        r = subprocess.run([tool, "makebanner", "-i", png, "-a", wav, "-o", out], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            sys.exit("bannertool failed:\n" + r.stdout + r.stderr)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--age", choices=("adult", "child"), default="adult")
    ap.add_argument("--raw", help="reuse a link_icon.bin instead of running the game")
    ap.add_argument("--mesh", help="reuse a link_mesh.bin (default with --raw: port/banner_local_mesh.bin if present)")
    ap.add_argument("--white", action="store_true", help="Link on white instead of a transparent background")
    ap.add_argument("--stereo", action="store_true",
                    help="experimental stereoscopic 3D banner (crashes the real HOME Menu, 2026-10-06)")
    ap.add_argument("--picture", action="store_true", help="with --stereo: Link as a picture instead of his 3D model")
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
    mesh = None if args.picture or not args.stereo else (args.mesh or os.path.join(REPO, "port", "banner_local_mesh.bin"))
    if mesh is not None and not os.path.exists(mesh):
        print("no model capture (%s): Link as a picture" % mesh)
        mesh = None
    if make_banner(link, args.banner_out, mesh, args.stereo, args.white):
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

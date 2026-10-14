#!/usr/bin/env python3
"""make_title_banner.py - HOME Menu icon (the Hylian Shield) and banner (the title-screen logo with an ocarina tune),
made from YOUR game data.

The shield icon, the logo and the music are Nintendo's, so these files cannot be committed: this tool makes them
locally - port/icon_local.png and port/banner_local.bnr, ignored by git. Makefile.3ds uses them when they exist, else
the original artwork port/icon.png and port/banner.bnr.

How:
- icon: the Hylian Shield's 3D model (object_gi_shield_2), rendered by the port from the front: builds it with
  GAME_EXTRA=-DPORT_SHIELDGEN (boots into Link's house; z_play.c draws the shield alone over a black and then a white
  screen and writes both frames to sdmc:/3ds/oot/shield_*.bin, as make_navi_icon.py does with Navi), on a light
  rounded square
- banner picture: the title screen's logo (object_mag: the shield logo with "THE LEGEND OF" and "OCARINA OF TIME"
  drawn over it with the colors z_en_mag.c uses), on a transparent background
- banner sound: builds the port with GAME_EXTRA=-DPORT_BANNER_SOUND=<sequence> (boots into Link's house, stops the
  music and plays that sequence, z_play.c), records it with the audio capture (sdmc:/3ds/oot/capture_audio ->
  oot_audio.wav) in Azahar, keeps its first seconds (the HOME Menu plays about 3) and rebuilds the normal ROM

usage: make_title_banner.py [--sound zelda|item|<NA_BGM_ name>] [--seconds 3.0] [--wav recorded.wav] [--raw-shield]
       (--wav, --raw-shield: reuse a recording / the last shield frames instead of running the game)
Needs: an extracted ROM (as for any build), Azahar, Python 3 + Pillow, bannertool
(https://github.com/diasurgical/bannertool) on PATH, in $BANNERTOOL or in $DEVKITPRO/tools/bin.
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile, time, wave

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_link_banner as L  # noqa: E402  (build, sh, bannertool, paths)
import make_navi_icon as N  # noqa: E402  (decode, matte)

ASSETS = os.path.join(L.REPO, "extracted/ntsc-1.0/assets")
SOUNDS = {"zelda": "NA_BGM_OCA_ZELDA", "item": "NA_BGM_ITEM_GET"}  # Zelda's Lullaby (song learned), item get


def tex(path):
    p = os.path.join(ASSETS, path)
    if not os.path.exists(p):
        sys.exit("%s not found: extract the ROM first (as for any build)" % p)
    return Image.open(p)


def run(extra, done):
    """builds a tool build, runs it in Azahar until every file in `done` exists on its SD card, rebuilds the normal ROM"""
    L.build(extra)
    try:
        L.sh("pkill -9 -f MacOS/azahar")
        time.sleep(1)
        app = L.sh("ls -d /Applications/*[Aa]zahar*/Azahar.app | head -1", capture_output=True, text=True).stdout.strip()
        if not app:
            sys.exit("Azahar not found in /Applications")
        L.sh("open -n '%s' --args '%s'" % (app, os.path.join(L.REPO, "build/3ds/oot.3ds")))
        deadline = time.time() + 180
        while not done():
            if time.time() > deadline:
                sys.exit("the tool build never finished (see boot.log in %s)" % L.SD)
            time.sleep(2)
        time.sleep(2)
    finally:
        L.sh("pkill -9 -f MacOS/azahar")
        L.build("")  # leave the normal ROM in build/3ds


SHIELD_FILES = ["shield_%d_%s.bin" % (t, c) for t in range(3) for c in ("black", "white")]


def render_shield():
    """the Hylian Shield's 3D model (z_play.c PortShieldGen), straight from the front, with transparency"""
    for f in SHIELD_FILES:
        if os.path.exists(os.path.join(L.SD, f)):
            os.remove(os.path.join(L.SD, f))
    run("-DPORT_SHIELDGEN -DPORT_START_ENTRANCE=%s -DPORT_START_AGE=1" % L.ENTRANCE,
        lambda: all(os.path.exists(os.path.join(L.SD, f)) for f in SHIELD_FILES))
    return N.matte(N.decode(os.path.join(L.SD, SHIELD_FILES[2])), N.decode(os.path.join(L.SD, SHIELD_FILES[3])))


def open_or_render_shield(raw):
    if raw:
        return N.matte(N.decode(os.path.join(L.SD, SHIELD_FILES[2])), N.decode(os.path.join(L.SD, SHIELD_FILES[3])))
    return render_shield()


def make_icon(shield):
    """the shield on a light square filling the whole icon, 48x48 (composed at 4x, then reduced). Fully opaque: the
    HOME Menu shows transparent icon pixels as black (rounded corners showed as black corners, hardware)"""
    n = 192
    bg = Image.new("RGBA", (n, n))
    d = ImageDraw.Draw(bg)
    for y in range(n):
        t = y / (n - 1)
        d.line([(0, y), (n, y)], fill=(int(250 - 30 * t), int(250 - 25 * t), int(255 - 10 * t), 255))
    shield = shield.crop(shield.getchannel("A").point(lambda v: 255 if v > 16 else 0).getbbox())
    scale = 172 / max(shield.size)
    shield = shield.resize((round(shield.width * scale), round(shield.height * scale)), Image.LANCZOS)
    bg.alpha_composite(shield, ((n - shield.width) // 2, (n - shield.height) // 2))
    return bg.resize((48, 48), Image.LANCZOS)


def tinted_i8(img, prim, env):
    """an I8 texture drawn with z_en_mag.c's combiner: color = (PRIM - ENV) * TEXEL0 + ENV, alpha = TEXEL0"""
    i = img.convert("L")
    out = Image.new("RGBA", i.size)
    px, src = out.load(), i.load()
    for y in range(i.height):
        for x in range(i.width):
            t = src[x, y] / 255.0
            px[x, y] = tuple(int((p - e) * t + e) for p, e in zip(prim, env)) + (src[x, y],)
    return out


def make_logo():
    """the title screen's logo at its N64 size (160x160): z_en_mag.c EnMag_DrawInner, English, not Master Quest"""
    logo = tex("objects/object_mag/gTitleZeldaShieldLogoTex.rgba32.png").convert("RGBA")
    legend = tex("objects/object_mag/gTitleTheLegendOfTextTex.i8.png")
    ocarina = tex("objects/object_mag/gTitleOcarinaOfTimeTMTextTex.i8.png")
    x0, y0 = 160 - 80, 100 - 80  # the logo is centered on (160, 100)
    for img, (sx, sy), (mx, my) in ((legend, (154, 73), (153, 72)), (ocarina, (152, 127), (151, 126))):
        logo.alpha_composite(tinted_i8(img, (0, 0, 0), (100, 0, 100)), (sx - x0, sy - y0))  # the shadow
        logo.alpha_composite(tinted_i8(img, (200, 200, 150), (100, 100, 50)), (mx - x0, my - y0))
    return logo


def record(seq):
    """runs the sound build in Azahar; returns the 20 s recording (32 kHz stereo WAV bytes)"""
    wav, flag = os.path.join(L.SD, "oot_audio.wav"), os.path.join(L.SD, "capture_audio")
    if os.path.exists(wav):
        os.remove(wav)
    open(flag, "w").close()
    try:
        run("-DPORT_BANNER_SOUND=%s -DPORT_START_ENTRANCE=%s -DPORT_START_AGE=1" % (seq, L.ENTRANCE),
            lambda: os.path.exists(wav) and os.path.getsize(wav) > 44 and open(wav, "rb").read(4) == b"RIFF")
    finally:
        os.remove(flag)
    return open(wav, "rb").read()


def cut(src, dst, seconds):
    """keeps `seconds` from the sound's start (after the first silence of 1 s or more), fading out the last 0.5 s"""
    w = wave.open(src)
    rate, ch = w.getframerate(), w.getnchannels()
    s = struct.unpack("<%dh" % (w.getnframes() * ch), w.readframes(w.getnframes()))
    frames = len(s) // ch
    loud = [max(abs(s[i * ch + c]) for c in range(ch)) > 200 for i in range(frames)]
    start, quiet = None, 0
    for i, on in enumerate(loud):  # (the recording starts with the boot's first audible frame)
        if on and quiet >= rate:
            start = i
            break
        quiet = 0 if on else quiet + 1
    if start is None:
        sys.exit("no sound found in the recording")
    n = min(int(seconds * rate), frames - start)
    fade = int(0.5 * rate)
    out = []
    for i in range(n):
        g = min(1.0, (n - i) / fade)
        out += [int(s[(start + i) * ch + c] * g) for c in range(ch)]
    o = wave.open(dst, "wb")
    o.setnchannels(ch)
    o.setsampwidth(2)
    o.setframerate(rate)
    o.writeframes(struct.pack("<%dh" % len(out), *out))
    o.close()


def make_banner(logo, wav, out):
    tool = L.bannertool()
    if tool is None:
        sys.exit("bannertool not found (PATH, $BANNERTOOL or $DEVKITPRO/tools/bin)")
    scale = 244 / logo.height  # composed at 512x256 (6 px margin), shown at 256x128
    big = logo.resize((int(logo.width * scale), int(logo.height * scale)), Image.LANCZOS)
    pic = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
    pic.alpha_composite(big, ((512 - big.width) // 2, (256 - big.height) // 2))
    pic = pic.resize((256, 128), Image.LANCZOS)
    pic.save(os.path.splitext(out)[0] + "_preview.png")
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, "banner.png")
        pic.save(png)
        r = subprocess.run([tool, "makebanner", "-i", png, "-a", wav, "-o", out], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            sys.exit("bannertool failed:\n" + r.stdout + r.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sound", default="zelda", help="zelda (Zelda's Lullaby), item (item get) or an NA_BGM_ name")
    ap.add_argument("--seconds", type=float, default=3.0, help="length kept (the HOME Menu plays about 3 s)")
    ap.add_argument("--wav", help="reuse a recording (oot_audio.wav) instead of running the game")
    ap.add_argument("--raw-shield", action="store_true", help="reuse the last shield_*.bin instead of running the game")
    ap.add_argument("--icon-out", default=os.path.join(L.REPO, "port/icon_local.png"))
    ap.add_argument("--banner-out", default=os.path.join(L.REPO, "port/banner_local.bnr"))
    args = ap.parse_args()
    for f in (args.icon_out, args.banner_out):  # keep what was there (e.g. the Navi icon, the Link banner)
        keep = "%s_before_title%s" % os.path.splitext(f)  # (still matched by .gitignore's icon_local*.png, banner_local*)
        if os.path.exists(f) and not os.path.exists(keep):
            shutil.copy(f, keep)
    icon = make_icon(open_or_render_shield(args.raw_shield))
    icon.save(args.icon_out)
    icon.resize((192, 192), Image.NEAREST).save(os.path.splitext(args.icon_out)[0] + "_preview.png")
    print("wrote %s" % args.icon_out)
    with tempfile.TemporaryDirectory() as tmp:
        rec = os.path.join(tmp, "recording.wav")
        if args.wav:
            shutil.copy(args.wav, rec)
        else:
            open(rec, "wb").write(record(SOUNDS.get(args.sound, args.sound)))
        snd = os.path.join(tmp, "banner.wav")
        cut(rec, snd, args.seconds)
        shutil.copy(snd, os.path.splitext(args.banner_out)[0] + "_sound.wav")
        make_banner(make_logo(), snd, args.banner_out)
    print("wrote %s (preview, sound: %s_preview.png, _sound.wav)" % (args.banner_out, os.path.splitext(args.banner_out)[0]))
    print("local only: the shield, the logo and the music are the game's, never commit these files")
    L.sh("make -f Makefile.3ds cci >/dev/null")  # the new files are newer than the icon and ROM: repacks only
    print("built the normal ROM with them: build/3ds/oot.3ds")


if __name__ == "__main__":
    main()

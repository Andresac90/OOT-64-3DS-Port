#!/usr/bin/env python3
"""fbdiff.py - renderer ground truth: the 3DS frame vs the N64 frame rendered by ares' RDP.

Once statediff shows both sides in the same game state, ares' framebuffer is the correct image, so
every remaining pixel difference is the port's renderer. For each tour capture this compares the N64
framebuffers (RGBA5551: the displayed one and both SysCfb buffers) with the 3DS color readbacks (the
last two finished frames, RGBA8, portrait, 2x2 supersampled), picks the pairing that matches best (the
frame-latency between the two is measured, not assumed), and reports per scene:
  mean abs error (0..255) and % of pixels off by > 48 on some channel,
plus build/statediff/fbdiff/<tour>_<i>.png = N64 | 3DS | difference heatmap.

usage: fbdiff.py --tour tour_child_40_101 [--only 0,5,7] [--png]
"""
import argparse, json, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import statediff as SD  # noqa: E402
import tour as T  # noqa: E402

OUT = os.path.join(SD.OUT, "fbdiff")


def n64_rgb(fb):
    px = struct.unpack(">%dH" % (320 * 240), fb)
    return [(((p >> 11) & 31) * 255 // 31, ((p >> 6) & 31) * 255 // 31, ((p >> 1) & 31) * 255 // 31) for p in px]


def ds_rgb(blob, order):
    """3DS RGBA8 portrait buffer -> 320x240 N64-space RGB (2x2 box filter over the supersamples)."""
    W, H = struct.unpack("<II", blob[:8])
    words = struct.unpack("<%dI" % (W * H), blob[8:8 + W * H * 4])
    sx, sy = H // 400, W // 240
    sh = [24, 16, 8] if order == "rgba" else [8, 16, 24]  # u32 value: 0xRRGGBBAA or 0xAABBGGRR
    out = []
    for y in range(240):
        for x in range(320):
            acc = [0, 0, 0]
            for oy in range(sy):
                u = min(W - 1, W - (y * sy + oy))
                for ox in range(sx):
                    w = words[((x + 40) * sx + ox) * W + u]
                    for c in range(3):
                        acc[c] += (w >> sh[c]) & 0xFF
            n = sx * sy
            out.append((acc[0] // n, acc[1] // n, acc[2] // n))
    return out


def score(a, b):
    err, bad = 0, 0
    for p, q in zip(a, b):
        e = max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2]))
        err += e
        bad += e > 48
    return err / len(a), 100.0 * bad / len(a)


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(v for px in row for v in px) for row in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tour", required=True, help="capture tag, e.g. tour_child_40_101")
    ap.add_argument("--only", default="")
    ap.add_argument("--png", action="store_true")
    a = ap.parse_args()
    n64 = T.load_caps(os.path.join(SD.OUT, "n64_%s.json" % a.tour))
    ds = T.load_caps(os.path.join(SD.OUT, "3ds_%s.json" % a.tour))
    only = set(int(x) for x in a.only.split(",")) if a.only else None
    os.makedirs(OUT, exist_ok=True)
    age = a.tour.split("_")[1]
    names = {i: t[0] for i, t in enumerate(T.retail_scene_entrances(age))}
    order = None
    for i in sorted(set(n64) & set(ds)):
        if only is not None and i not in only:
            continue
        nb = {k: n64[i].get(k) for k in ("fbdisp", "fb0", "fb1") if n64[i].get(k)}
        db = {k: ds[i].get(k) for k in ("color0", "color1") if ds[i].get(k)}
        if not nb or not db:
            continue
        orders = [order] if order else ["rgba", "abgr"]
        best = None
        n_rgb = {k: n64_rgb(v) for k, v in nb.items()}
        for o in orders:
            for dk, dv in db.items():
                d_rgb = ds_rgb(dv, o)
                for nk, nv in n_rgb.items():
                    s = score(nv, d_rgb)
                    if best is None or s[0] < best[0][0]:
                        best = (s, o, nk, dk, nv, d_rgb)
        (mae, badpct), order, nk, dk, nv, dv = best
        print("  %-3d %-34s mean err %5.1f   %5.1f%% px off   (N64 %s vs 3DS %s, %s)" %
              (i, names.get(i, "?"), mae, badpct, nk, dk, order), flush=True)
        if a.png:
            rows = []
            for y in range(240):
                row = []
                for x in range(320):
                    row.append(nv[y * 320 + x])
                for x in range(320):
                    row.append(dv[y * 320 + x])
                for x in range(320):
                    p, q = nv[y * 320 + x], dv[y * 320 + x]
                    e = min(255, 2 * max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2])))
                    row.append((e, 0 if e < 96 else e // 2, 0))
                rows.append(row)
            write_png(os.path.join(OUT, "%s_%d.png" % (a.tour, i)), 960, 240, rows)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""flaterr.py - rank fbdiff scenes by FLAT-AREA error: pixels that differ from the N64 where both images
are locally smooth. Edge pixels (2x supersampled AA vs the N64's 1x rasterization) and N64 dither noise
dominate fbdiff's mean error but are inherent; flat-area error is what real color/material/texture
bugs produce (a wrong combiner, texture format, fog, lighting...).

Reads the N64 | 3DS | heatmap PNGs fbdiff --png wrote (build/statediff/fbdiff/<tag>_<i>.png).
usage: flaterr.py TAG [--top N] [--thresh 40]
"""
import argparse, glob, os, re, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PNG_DIR = os.path.join(os.path.dirname(os.path.dirname(HERE)), "build", "statediff", "fbdiff")


def read_png(path):
    d = open(path, "rb").read()
    i, idat, w, h, ct = 8, b"", 0, 0, 2
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h = struct.unpack(">II", c[:8])
            ct = c[9]
        elif t == b"IDAT":
            idat += c
        i += 12 + n
    raw, bpp = zlib.decompress(idat), (3 if ct == 2 else 4)
    stride, rows, prev = w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def lum(rows, bpp, x, y):
    p = rows[y][x * bpp:x * bpp + 3]
    return p[0] + p[1] + p[2]


def analyse(path, thresh):
    w, h, bpp, rows = read_png(path)
    pw = w // 3  # N64 | 3DS | heatmap
    flat_bad, flat_total, sums = 0, 0, [0] * 6
    for y in range(1, h - 1):
        for x in range(1, pw - 1):
            flat = True
            for ox in (0, pw):  # both images smooth in the 3x3 neighbourhood
                vals = [lum(rows, bpp, x + ox + dx, y + dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
                if max(vals) - min(vals) > 60:
                    flat = False
                    break
            if not flat:
                continue
            flat_total += 1
            n = rows[y][x * bpp:x * bpp + 3]
            d = rows[y][(x + pw) * bpp:(x + pw) * bpp + 3]
            if max(abs(n[c] - d[c]) for c in range(3)) > thresh:
                flat_bad += 1
                for c in range(3):
                    sums[c] += n[c]
                    sums[3 + c] += d[c]
    avg = ["#%02x%02x%02x" % tuple(s // flat_bad for s in sums[k:k + 3]) for k in (0, 3)] if flat_bad else ["-", "-"]
    return flat_bad, flat_total, avg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--thresh", type=int, default=40)
    a = ap.parse_args()
    res = []
    for p in glob.glob(os.path.join(PNG_DIR, "%s_*.png" % a.tag)):
        m = re.search(r"_(\d+)\.png$", p)
        if not m:
            continue
        bad, tot, avg = analyse(p, a.thresh)
        res.append((bad, int(m.group(1)), tot, avg))
    res.sort(reverse=True)
    print("scene  flat-bad-px  (of flat)  N64 avg -> 3DS avg")
    for bad, i, tot, avg in res[:a.top]:
        print("%5d  %8d  (%6d)  %s -> %s" % (i, bad, tot, avg[0], avg[1]))
    print("total flat-bad px over %d scenes: %d" % (len(res), sum(r[0] for r in res)))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""zcal.py - calibrate/verify the 3DS depth -> N64 z-buffer conversion against ares' real z-buffer.

Inputs are statediff captures of the same frame: n64_<e>_f<N>_zbuf.bin (gZBuffer, 320x240 BE u16, as the
RDP wrote it) and 3ds_<e>_f<N>_depth.bin (u32 w, u32 h, D24S8 words, linear). For every candidate
orientation of the portrait 3DS buffer and depth-bit layout it predicts the N64 screen z of each
pixel and reports how well it matches, so port/src/zbuffer_port.c uses a measured mapping.

usage: zcal.py --entrance 0x000 --frame 100
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(os.path.dirname(HERE)), "build/statediff")
# z_kankyo.c sZBufValConversionTable: (mantissaShift, base) per exponent
TABLE = [(6, 0x0000 << 3), (5, 0x4000 << 3), (4, 0x6000 << 3), (3, 0x7000 << 3),
         (2, 0x7800 << 3), (1, 0x7C00 << 3), (0, 0x7E00 << 3), (0, 0x7F00 << 3)]


def n64_fixed(word):
    """Environment_ZBufValToFixedPoint(gZBuffer[y][x] << 2) >> 3: the value the game compares against."""
    v = word << 2
    shift, base = TABLE[(v >> 15) & 7]
    return ((((v >> 4) & 0x7FF) << shift) + base) >> 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entrance", default="")
    ap.add_argument("--frame", type=int, default=100)
    a = ap.parse_args()
    tag = "%s_f%d" % (a.entrance or "default", a.frame)
    zb = open(os.path.join(OUT, "n64_%s_zbuf.bin" % tag), "rb").read()
    dp = open(os.path.join(OUT, "3ds_%s_depth.bin" % tag), "rb").read()
    W, H = struct.unpack("<II", dp[:8])
    words = struct.unpack("<%dI" % (W * H), dp[8:8 + W * H * 4])
    n64 = [[n64_fixed(struct.unpack(">H", zb[(y * 320 + x) * 2:(y * 320 + x) * 2 + 2])[0]) for x in range(320)]
           for y in range(240)]
    far = n64_fixed(0xFFFC)
    sx, sy = H // 400, W // 240  # supersampling factor along window x (buffer height) and y (width)

    def depth_bits(w, layout):
        return (w & 0xFFFFFF) if layout == "low24" else (w >> 8)

    results = []
    for layout in ("low24", "high24"):
        for flip_u in (False, True):      # buffer column (u, 0..W-1) <- window y
            for flip_v in (False, True):  # buffer row (v, 0..H-1)   <- window x
                errs, n_far_ok, n = [], 0, 0
                for y in range(0, 240, 4):
                    for x in range(0, 320, 4):
                        wx, wy = (x + 40) * sx + sx // 2, y * sy + sy // 2  # N64 pixel -> window (pillarbox 40)
                        u = (W - 1 - wy) if flip_u else wy
                        v = (H - 1 - wx) if flip_v else wx
                        d = depth_bits(words[v * W + u], layout)
                        pred = d * 0x7FC0 // 0xFFFFFF
                        ref = n64[y][x]
                        n += 1
                        if ref >= far - 8 and d >= 0xFFFF00:
                            n_far_ok += 1
                        elif ref < far - 8 and d < 0xFFFF00:
                            errs.append(abs(pred - ref))
                errs.sort()
                med = errs[len(errs) // 2] if errs else None
                agree = (len(errs) + n_far_ok) / n
                results.append((agree, -(med or 1e9), layout, flip_u, flip_v, med, len(errs), n_far_ok, n))
    results.sort(reverse=True)
    print("depth buffer %dx%d (supersample x%d/y%d), N64 far value %d" % (W, H, sx, sy, far))
    for agree, _, layout, fu, fv, med, ne, nf, n in results[:4]:
        print("  %-6s flip_u=%-5s flip_v=%-5s  near/far agree %5.1f%%  median |pred-N64| %s over %d near px (+%d far)" %
              (layout, fu, fv, 100 * agree, med, ne, nf))
    best = results[0]
    # error distribution for the best mapping, in N64 screen-z units (0..0x7FC0)
    layout, fu, fv = best[2], best[3], best[4]
    errs = []
    for y in range(0, 240, 2):
        for x in range(0, 320, 2):
            wx, wy = (x + 40) * sx + sx // 2, y * sy + sy // 2
            u = (W - 1 - wy) if fu else wy
            v = (H - 1 - wx) if fv else wx
            d = depth_bits(words[v * W + u], layout)
            if n64[y][x] < far - 8 and d < 0xFFFF00:
                errs.append(d * 0x7FC0 // 0xFFFFFF - n64[y][x])
    errs.sort()
    if errs:
        pct = lambda p: errs[min(len(errs) - 1, int(p * len(errs)))]
        print("best: %s flip_u=%s flip_v=%s; signed error pred-N64 percentiles 5/25/50/75/95: %s" %
              (layout, fu, fv, [pct(p) for p in (0.05, 0.25, 0.5, 0.75, 0.95)]))


if __name__ == "__main__":
    main()

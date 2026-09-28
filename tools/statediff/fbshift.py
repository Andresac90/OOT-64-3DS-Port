#!/usr/bin/env python3
"""fbshift.py - measure the sub-pixel offset between the 3DS render and the N64 frame.

Samples the 3DS color readback (2x2 supersampled, portrait) at every (dx, dy) supersample shift from the
physical mapping and reports the mean error per shift over a region, so rendering offsets are measured.
usage: fbshift.py --tour TAG --index I [--region y0,y1,x0,x1]
"""
import argparse, os, struct, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tour as T, fbdiff as F  # noqa: E402

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tour", required=True)
    ap.add_argument("--index", type=int, required=True)
    ap.add_argument("--region", default="30,200,20,300")
    a = ap.parse_args()
    out = os.path.join(HERE, "..", "..", "build/statediff")
    n = T.load_caps(os.path.join(out, "n64_%s.json" % a.tour))[a.index]
    d = T.load_caps(os.path.join(out, "3ds_%s.json" % a.tour))[a.index]
    y0, y1, x0, x1 = map(int, a.region.split(","))
    best = None
    for nk in ("fbdisp", "fb0", "fb1"):
        if not n.get(nk):
            continue
        N = np.array(F.n64_rgb(n[nk]), np.int32).reshape(240, 320, 3)
        for dk in ("color0", "color1"):
            blob = d[dk]
            W, H = struct.unpack("<II", blob[:8])
            w = np.frombuffer(blob[8:8 + W * H * 4], "<u4").reshape(H, W)
            img = np.stack([(w >> 24) & 255, (w >> 16) & 255, (w >> 8) & 255], -1).astype(np.int32)
            res = {}
            for dx in (-2, -1, 0, 1):
                for dy in (-2, -1, 0, 1):
                    ys, xs = np.arange(y0, y1), np.arange(x0, x1)
                    acc = 0
                    for ox in (0, 1):
                        for oy in (0, 1):
                            rows = np.clip((xs + 40) * 2 + ox + dx, 0, H - 1)[None, :]
                            cols = np.clip(W - 1 - (ys * 2 + oy + dy), 0, W - 1)[:, None]
                            acc = acc + img[rows, cols]
                    D = acc // 4
                    res[(dx, dy)] = np.abs(D - N[y0:y1, x0:x1]).max(-1).mean()
            m = min(res.values())
            if best is None or m < best[0]:
                best = (m, nk, dk, res)
    m, nk, dk, res = best
    print("N64 %s vs 3DS %s: mean error by supersample shift (dx right, dy down)" % (nk, dk))
    for dy in (-2, -1, 0, 1):
        print("  dy %+d: " % dy + "  ".join("dx %+d %5.2f" % (dx, res[(dx, dy)]) for dx in (-2, -1, 0, 1)))

if __name__ == "__main__":
    main()

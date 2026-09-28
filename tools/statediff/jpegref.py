#!/usr/bin/env python3
"""jpegref.py - baseline JPEG decoder with every stage exposed, to match the N64's decoder.

OoT's pre-rendered room backgrounds are JPEGs the N64 decodes with its own code (Huffman on the CPU,
z_jpeg.c; dequantize/IDCT/YUV->RGB on the RSP, njpgdspMain). In COPY mode the decoded texels land in
the framebuffer unchanged, so ares' framebuffer is the N64 decoder's output. This decodes a JPEG into
Y/Cb/Cr planes (coefficients -> IDCT) and converts with selectable upsampling / conversion, so
variants can be scored against those pixels (--tour/--scene) instead of guessed.

usage: jpegref.py --jpg FILE --tour tour_child_40_4 --index 0 [--region y0,y1,x0,x1]
"""
import argparse, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

ZIGZAG = np.array([0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,
                   6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45,
                   38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63])


class Bits:
    def __init__(self, data):
        self.d, self.p, self.acc, self.n = data, 0, 0, 0

    def bit(self):
        if self.n == 0:
            b = self.d[self.p]
            self.p += 1
            if b == 0xFF:
                nxt = self.d[self.p]
                if nxt == 0:
                    self.p += 1
                elif 0xD0 <= nxt <= 0xD7:  # restart markers are handled by the caller
                    pass
            self.acc, self.n = b, 8
        self.n -= 1
        return (self.acc >> self.n) & 1

    def bits(self, k):
        v = 0
        for _ in range(k):
            v = (v << 1) | self.bit()
        return v

    def align_restart(self):
        self.n = 0
        if self.d[self.p] == 0xFF and 0xD0 <= self.d[self.p + 1] <= 0xD7:
            self.p += 2


def build_huff(counts, symbols):
    table, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            table[(length, code)] = symbols[k]
            code += 1
            k += 1
        code <<= 1
    return table


def decode_huff(bits, table):
    code = 0
    for length in range(1, 17):
        code = (code << 1) | bits.bit()
        if (length, code) in table:
            return table[(length, code)]
    raise ValueError("bad huffman code")


def extend(v, t):
    return v - (1 << t) + 1 if v < (1 << (t - 1)) else v


def parse(data):
    """-> dict with quant tables, components, per-component coefficient blocks (dequantized later)"""
    p, qt, ht, comps, restart = 2, {}, {}, [], 0
    while True:
        assert data[p] == 0xFF
        m = data[p + 1]
        ln = struct.unpack(">H", data[p + 2:p + 4])[0]
        seg = data[p + 4:p + 2 + ln]
        if m == 0xDB:
            i = 0
            while i < len(seg):
                pq, tq = seg[i] >> 4, seg[i] & 15
                vals = struct.unpack(">64H", seg[i + 1:i + 129]) if pq else tuple(seg[i + 1:i + 65])
                q = np.zeros(64, np.int32)
                q[ZIGZAG] = vals
                qt[tq] = q.reshape(8, 8)
                i += 1 + (128 if pq else 64)
        elif m == 0xC0:
            h, w, nc = struct.unpack(">HHB", seg[1:6])
            for c in range(nc):
                cid, hv, tq = seg[6 + 3 * c:9 + 3 * c]
                comps.append({"id": cid, "h": hv >> 4, "v": hv & 15, "tq": tq})
        elif m == 0xC4:
            i = 0
            while i < len(seg):
                tc, th = seg[i] >> 4, seg[i] & 15
                counts = seg[i + 1:i + 17]
                n = sum(counts)
                ht[(tc, th)] = build_huff(counts, seg[i + 17:i + 17 + n])
                i += 17 + n
        elif m == 0xDD:
            restart = struct.unpack(">H", seg[:2])[0]
        elif m == 0xDA:
            ns = seg[0]
            for c in range(ns):
                cid, t = seg[1 + 2 * c], seg[2 + 2 * c]
                for comp in comps:
                    if comp["id"] == cid:
                        comp["td"], comp["ta"] = t >> 4, t & 15
            scan = data[p + 2 + ln:]
            break
        p += 2 + ln
    hmax = max(c["h"] for c in comps)
    vmax = max(c["v"] for c in comps)
    mcux, mcuy = (w + 8 * hmax - 1) // (8 * hmax), (h + 8 * vmax - 1) // (8 * vmax)
    for c in comps:
        c["coef"] = np.zeros((mcuy * c["v"], mcux * c["h"], 64), np.int32)
        c["pred"] = 0
    bits = Bits(scan)
    for my in range(mcuy):
        for mx in range(mcux):
            k = my * mcux + mx
            if restart and k and k % restart == 0:
                bits.align_restart()
                for c in comps:
                    c["pred"] = 0
            for c in comps:
                for by in range(c["v"]):
                    for bx in range(c["h"]):
                        blk = np.zeros(64, np.int32)
                        t = decode_huff(bits, ht[(0, c["td"])])
                        c["pred"] += extend(bits.bits(t), t) if t else 0
                        blk[0] = c["pred"]
                        i = 1
                        while i < 64:
                            rs = decode_huff(bits, ht[(1, c["ta"])])
                            r, s = rs >> 4, rs & 15
                            if s == 0:
                                if r == 15:
                                    i += 16
                                    continue
                                break
                            i += r
                            blk[i] = extend(bits.bits(s), s)
                            i += 1
                        c["coef"][my * c["v"] + by, mx * c["h"] + bx] = blk
    return {"w": w, "h": h, "qt": qt, "comps": comps, "hmax": hmax, "vmax": vmax}


def idct_float(blocks):
    """blocks: (..., 8, 8) dequantized natural-order coefficients -> samples (float, no level shift)"""
    x = np.arange(8)
    c = np.where(x == 0, 1 / np.sqrt(2), 1.0)
    M = (c[:, None] * np.cos((2 * x[None, :] + 1) * x[:, None] * np.pi / 16)) / 2  # [u, x]
    return np.einsum("ux,...uv,vy->...xy", M, blocks, M)


def planes(j):
    out = []
    for c in j["comps"]:
        coef = c["coef"]
        nat = np.zeros(coef.shape, np.int32)
        nat[..., ZIGZAG] = coef
        deq = nat.reshape(coef.shape[:2] + (8, 8)) * j["qt"][c["tq"]]
        s = idct_float(deq.astype(np.float64))  # (by, bx, 8, 8)
        by, bx = s.shape[:2]
        out.append((s.transpose(0, 2, 1, 3).reshape(by * 8, bx * 8), c["h"], c["v"]))
    return out


def upsample(p, h, v, hmax, vmax, mode):
    fy, fx = vmax // v, hmax // h
    if fy == 1 and fx == 1:
        return p
    if mode == "nearest":
        return np.repeat(np.repeat(p, fy, 0), fx, 1)
    # "linear": centered bilinear (libjpeg's fancy upsampling is equivalent: 3/4, 1/4 weights)
    ys = (np.arange(p.shape[0] * fy) + 0.5) / fy - 0.5
    xs = (np.arange(p.shape[1] * fx) + 0.5) / fx - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, p.shape[0] - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, p.shape[1] - 1)
    y1, x1 = np.clip(y0 + 1, 0, p.shape[0] - 1), np.clip(x0 + 1, 0, p.shape[1] - 1)
    wy, wx = np.clip(ys - np.floor(ys), 0, 1)[:, None], np.clip(xs - np.floor(xs), 0, 1)[None, :]
    a = p[y0][:, x0] * (1 - wx) + p[y0][:, x1] * wx
    b = p[y1][:, x0] * (1 - wx) + p[y1][:, x1] * wx
    return a * (1 - wy) + b * wy


def to_rgb(j, up="linear", coefs=(1.402, 0.344136, 0.714136, 1.772), level=128.0):
    pl = planes(j)
    Y, Cb, Cr = [upsample(p, h, v, j["hmax"], j["vmax"], up)[:j["h"], :j["w"]] for p, h, v in pl]
    Y = Y + level
    r = Y + coefs[0] * Cr
    g = Y - coefs[1] * Cb - coefs[2] * Cr
    b = Y + coefs[3] * Cb
    return np.stack([r, g, b], -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jpg", required=True)
    ap.add_argument("--tour", required=True)
    ap.add_argument("--index", type=int, default=0)
    ap.add_argument("--fb", default="fbdisp")
    ap.add_argument("--region", default="60,200,20,300")
    a = ap.parse_args()
    import tour as T
    import fbdiff as F
    n = T.load_caps(os.path.join(os.path.dirname(HERE), "..", "build/statediff/n64_%s.json" % a.tour))[a.index]
    N = np.array(F.n64_rgb(n[a.fb]), np.int32).reshape(240, 320, 3)
    y0, y1, x0, x1 = map(int, a.region.split(","))
    j = parse(open(a.jpg, "rb").read())

    def score(img, name):
        D = (np.clip(np.floor(img + 0.5), 0, 255).astype(np.int32) >> 3) * 255 // 31
        e = np.abs(D[y0:y1, x0:x1] - N[y0:y1, x0:x1]).max(axis=2)
        m = e < 80
        exact = (e[m] == 0).mean()
        print("%-40s mean %.2f  exact %.1f%%  inliers %.0f%%" % (name, e[m].mean(), 100 * exact, 100 * m.mean()))

    for up in ("nearest", "linear"):
        score(to_rgb(j, up), "float idct, %s chroma, JFIF" % up)
        score(to_rgb(j, up, (1.4, 0.343, 0.711, 1.765)), "float idct, %s chroma, 601 approx" % up)


if __name__ == "__main__":
    main()

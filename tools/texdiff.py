#!/usr/bin/env python3
"""texdiff.py — diff the port's decoded textures against the decomp's golden PNGs.

Workflow (no guessing: the decomp extractor's PNGs are ground truth for texture decoding):
  make -f Makefile.3ds cci PORT_EXTRA=-DPORT_TEXDUMP   # (touch port/src/gfx/gfx_pc.c first)
  tools/regress.sh texdump                              # boots, writes sdmc:/3ds/oot/texdump.bin
  python3 tools/texdiff.py                              # per-texture verdicts

For every dumped texture whose address is the START of a named asset symbol with a golden PNG,
reports MATCH / MISMATCH. On mismatch it tests the byte-order hypothesis "each 8-byte group is
reversed" (TEXB's ^7) by un-reversing pixel groups and re-comparing, so the verdict says which
fix (if any) would make the decode exact.
"""
import os, sys, struct, subprocess, collections
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(REPO, "build/3ds/oot.elf")
ASSETS = os.path.join(REPO, "extracted/ntsc-1.0/assets")
DUMP = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/oot/texdump.bin")
NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
FMT = {0: "RGBA", 1: "YUV", 2: "CI", 3: "IA", 4: "I"}
BITS = {0: 4, 1: 8, 2: 16, 3: 32}
TOL = 8  # per-channel tolerance (N64 bit-depth scaling differences)


def read_dump(path):
    out = []
    data = open(path, "rb").read()
    o = 0
    while o + 20 <= len(data):
        magic, addr, fs, w, h = struct.unpack_from("<5I", data, o)
        if magic != 0x44584554:
            break
        o += 20
        n = w * h * 4
        out.append((addr, fs & 0xFF, (fs >> 8) & 0xFF, w, h, data[o:o + n]))
        o += n
    return out


def symbol_starts():
    starts = {}
    for line in subprocess.run([NM, "-S", ELF], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 4:
            starts[int(p[0], 16)] = p[3]
    return starts


def golden_index():
    idx = {}
    for root, _, files in os.walk(ASSETS):
        for f in files:
            if f.endswith(".png"):
                idx.setdefault(f.split(".")[0], os.path.join(root, f))
    return idx


def golden_rgba(path):
    im = Image.open(path)
    if im.mode == "P" or im.mode == "PA":
        im = im.convert("RGBA")
    elif im.mode == "L":
        im = im.convert("LA")
    if im.mode == "LA":
        l, a = im.split()
        im = Image.merge("RGBA", (l, l, l, a))
    else:
        im = im.convert("RGBA")
    return im.size, im.tobytes()


def compare(a, b):
    """fraction of pixels within tolerance, max channel error"""
    npx = len(a) // 4
    ok = 0
    worst = 0
    for i in range(npx):
        d = max(abs(a[4 * i + c] - b[4 * i + c]) for c in range(4))
        worst = max(worst, d)
        if d <= TOL:
            ok += 1
    return ok / npx if npx else 0.0, worst


def unreverse_groups(px, bits):
    """undo a per-8-byte reversal: 8 bytes = 64 bits = 64/bits pixels per group"""
    per = max(1, 64 // bits)
    out = bytearray(len(px))
    npx = len(px) // 4
    for g in range(0, npx, per):
        grp = [px[4 * i:4 * i + 4] for i in range(g, min(g + per, npx))]
        for j, p in enumerate(reversed(grp)):
            out[4 * (g + j):4 * (g + j) + 4] = p
    return bytes(out)


def main():
    dump = read_dump(sys.argv[1] if len(sys.argv) > 1 else DUMP)
    starts, gold = symbol_starts(), golden_index()
    counts = collections.Counter()
    for addr, fmt, siz, w, h, px in dump:
        region = "linear/DMA" if addr >= 0x10000000 else "binary"
        name = starts.get(addr)
        kind = "%s%d" % (FMT.get(fmt, "?"), BITS.get(siz, 0))
        if name is None or name not in gold:
            counts["no-golden (%s)" % region] += 1
            continue
        (gw, gh), g = golden_rgba(gold[name])
        if fmt == 4:  # N64 I4/I8: the RDP replicates intensity into alpha
            g = bytearray(g)
            for i in range(0, len(g), 4):
                g[i + 3] = g[i]
            g = bytes(g)
        if (gw, gh) != (w, h):
            counts["dims-differ"] += 1
            print("DIMS      %-38s %-6s port %dx%d vs golden %dx%d" % (name, kind, w, h, gw, gh))
            continue
        frac, worst = compare(px, g)
        if frac > 0.99:
            counts["MATCH"] += 1
            print("MATCH     %-38s %-6s %dx%d" % (name, kind, w, h))
            continue
        frac2, _ = compare(unreverse_groups(px, BITS.get(siz, 8)), g)
        verdict = "MISMATCH*^7" if frac2 > 0.99 else "MISMATCH"
        counts[verdict] += 1
        print("%-9s %-38s %-6s %dx%d  ok=%.0f%% worst=%d  (if 8-byte groups un-reversed: ok=%.0f%%)"
              % (verdict, name, kind, w, h, frac * 100, worst, frac2 * 100))
    print("\nSUMMARY:", dict(counts))
    print("MISMATCH*^7 = decode becomes exact once the ^7 byte swizzle is undone for that texture")


if __name__ == "__main__":
    main()

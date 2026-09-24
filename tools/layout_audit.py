#!/usr/bin/env python3
"""layout_audit.py — verify every VROM-mapped native asset segment keeps its ROM layout.

The DMA shim serves a ROM read inside a natively-linked segment as native_base + rom_offset
(port/src_gen/vrom_map.c). That is only correct if the linker kept the segment's symbols in
declaration (== ROM) order, contiguously. This checks, per vrom_map entry: take the native base
symbol, find the asset source file that defines it, list that file's top-level symbols in
declaration order, and compare with their native address order in build/3ds/oot.elf.
"""
import os, re, subprocess, collections

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
ELF = os.path.join(REPO, "build/3ds/oot.elf")
VROM = os.path.join(REPO, "port/src_gen/vrom_map.c")
ROOTS = [os.path.join(REPO, d) for d in ("assets", "extracted/ntsc-1.0/assets", "build/ntsc-1.0/assets")]
DECL = re.compile(r"^\s*(?:static\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[", re.M)


def main():
    addr = {}
    for line in subprocess.run([NM, ELF], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            addr[p[2]] = int(p[0], 16)
    bases = re.findall(r"\{\s*_(\w+)SegmentRomStart,\s*_\w+SegmentRomEnd,\s*(\w+)\s*\}", open(VROM).read())
    # index: symbol -> defining .c file (declaration order preserved)
    defs = {}
    for root in ROOTS:
        for dp, _, files in os.walk(root):
            for f in files:
                if f.endswith(".c"):
                    path = os.path.join(dp, f)
                    for name in DECL.findall(open(path, errors="ignore").read()):
                        defs.setdefault(name, path)
    stats = collections.Counter()
    bad = []
    for seg, base in bases:
        path = defs.get(base)
        if path is None or base not in addr:
            stats["unchecked"] += 1
            continue
        order = [n for n in DECL.findall(open(path, errors="ignore").read()) if n in addr]
        if len(order) < 2:
            stats["trivial"] += 1
            continue
        addrs = [addr[n] for n in order]
        ok = all(a < b for a, b in zip(addrs, addrs[1:])) and addr[base] == min(addrs)
        stats["ok" if ok else "MISORDERED"] += 1
        if not ok:
            bad.append(seg)
    print("segments:", dict(stats))
    if bad:
        print("MISORDERED (DMA reads of these return wrong data):", bad[:40], "..." if len(bad) > 40 else "")


if __name__ == "__main__":
    main()

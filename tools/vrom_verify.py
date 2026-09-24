#!/usr/bin/env python3
"""vrom_verify.py — prove every VROM-mapped native segment matches the ROM layout (ground truth:
the decomp's extraction XMLs, which give each asset's exact ROM Offset within its segment).

For each segment served by port/src_gen/vrom_map.c:
  delta(sym) = native_address(sym) - xml_offset(sym)
All deltas must be identical (proves order + contiguity + no gaps), and must equal the map's base
address. Prints per-segment verdicts; --emit writes corrected entries as
  { _segSegmentRomStart, _segSegmentRomEnd, (char*)SYM - 0xOFF }
to port/src_gen/vrom_map_fix.inc for review.
"""
import os, re, sys, subprocess, collections, xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
ELF = os.path.join(REPO, "build/3ds/oot.elf")
VROM = os.path.join(REPO, "port/src_gen/vrom_map.c")
XMLDIR = os.path.join(REPO, "assets/xml")


def version_xmls():
    """Only the XMLs baseroms/ntsc-1.0/config.yml assigns to this version (PAL/GC variants of the
    same segment exist with different offsets; merging them made correct segments look wrong)."""
    cfg = open(os.path.join(REPO, "baseroms/ntsc-1.0/config.yml")).read()
    return sorted(set(os.path.join(REPO, p.strip()) for p in re.findall(r"xml_path:\s*(\S+)", cfg)))


def xml_offsets():
    segs = collections.defaultdict(dict)
    for path in version_xmls():
        if not os.path.exists(path):
            continue
        if True:
            try:
                root = ET.parse(path).getroot()
            except ET.ParseError:
                continue
            for fe in root.iter("File"):
                seg = fe.get("Name")
                for el in fe.iter():
                    n, off = el.get("Name"), el.get("Offset")
                    if n and off and el is not fe:
                        if re.fullmatch(r"0x[0-9A-Fa-f]+", off): segs[seg][n] = int(off, 16)
    return segs


def main():
    addr = {}
    for line in subprocess.run([NM, ELF], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            addr[p[2]] = int(p[0], 16)
    entries = re.findall(r"\{\s*_(\w+)SegmentRomStart,\s*_\w+SegmentRomEnd,\s*([^}]+?)\s*\}", open(VROM).read())
    xo = xml_offsets()
    stats = collections.Counter()
    fixes = []
    for seg, native in entries:
        offs = {n: o for n, o in xo.get(seg, {}).items() if n in addr}
        if len(offs) < 1:
            stats["no-xml-data"] += 1
            continue
        deltas = collections.Counter(addr[n] - o for n, o in offs.items())
        good, votes = deltas.most_common(1)[0]
        consistent = len(deltas) == 1
        m = re.match(r"^\(char\s*\*\)\s*(\w+)\s*-\s*(0x[0-9a-fA-F]+)$", native)
        cur = (addr.get(m.group(1), -1) - int(m.group(2), 16)) if m else addr.get(native, -1)
        if consistent and cur == good:
            stats["OK"] += 1
            continue
        if not consistent:
            stats["INCONSISTENT"] += 1
            bad = [n for n, o in offs.items() if addr[n] - o != good]
            print("INCONSISTENT %-28s %d/%d assets agree; e.g. %s" % (seg, votes, len(offs), bad[:3]))
        else:
            stats["WRONG-BASE"] += 1
            print("WRONG-BASE   %-28s map base %08x, ground truth %08x (off by %+d)" % (seg, cur, good, cur - good))
        sym, off = min(offs.items(), key=lambda kv: kv[1])
        fixes.append("    { _%sSegmentRomStart, _%sSegmentRomEnd, (char*)%s - 0x%X }," % (seg, seg, sym, off))
    print("\nSUMMARY:", dict(stats))
    if "--emit" in sys.argv:
        open(os.path.join(REPO, "port/src_gen/vrom_map_fix.inc"), "w").write("\n".join(fixes) + "\n")
        print("wrote port/src_gen/vrom_map_fix.inc (%d entries)" % len(fixes))


if __name__ == "__main__":
    main()

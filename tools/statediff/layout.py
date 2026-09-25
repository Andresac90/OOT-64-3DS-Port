#!/usr/bin/env python3
"""layout.py - flatten the game's structs into leaf fields using DWARF from layout_probe.c.

Output (JSON, cached at build/statediff/layout.json):
  { "PlayState": {"size": N, "fields": [[path, offset, size, kind, is_pad], ...]}, ... }
kind: "u" unsigned int, "s" signed int, "f" float, "d" double, "p" pointer,
      "b" bitfield (extra items: bit_pos, bit_size),
      "U" union (extra item: [[view_name, [fields relative to the union]], ...]).
is_pad: field name looks like decomp padding/unknown (unk_*, pad*), reported separately.

The probe is compiled twice with the 3DS game flags: as-is (3DS layout) and without __3DS__ (N64
layout; both targets are 32-bit with the same alignment, only the port's #ifdef __3DS__ field
reorders differ). Each field carries its N64 offset as its last item. main() checks the N64 sizes
against the decomp's own "} Name; // size = 0x..." annotations.
"""
import json, os, re, subprocess, sys
from elftools.elf.elffile import ELFFile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.join(REPO, "build/statediff")
PROBE_SRC = os.path.join(REPO, "tools/statediff/layout_probe.c")
PROBE_OBJ = os.path.join(OUT_DIR, "layout_probe.o")
CC = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc"
ROOT_TYPES = ["PlayState", "SaveContext", "Actor", "Player", "EnvironmentContext"]
PAD_RE = re.compile(r"(^|\.)(unk|pad|filler|dummy)", re.I)
MAX_EXPAND = 4096  # arrays longer than this are compared as one blob of elements anyway


def game_cflags():
    out = subprocess.run(["make", "-f", "Makefile.3ds", "-n", "-B", "build/3ds/src/code/z_kankyo.o"],
                         cwd=REPO, capture_output=True, text=True).stdout
    line = next(l for l in out.splitlines() if "z_kankyo.c" in l and l.startswith("/"))
    args = line.split()[1:]
    return args[:args.index("-MMD")] if "-MMD" in args else args


def build_probe(obj, n64):
    """n64=True compiles the same headers without __3DS__: the N64 view of every struct (port-only
    field reorders such as the little-endian fixes live under #ifdef __3DS__)."""
    os.makedirs(OUT_DIR, exist_ok=True)
    flags = [f for f in game_cflags() if not (n64 and f == "-D__3DS__")]
    if n64:
        flags.append("-fno-short-enums")  # IDO: every enum is a 4-byte int (arm-none-eabi GCC: 1/2/4)
    subprocess.run([CC] + flags + ["-g", "-O0", "-c", PROBE_SRC, "-o", obj], cwd=REPO, check=True)


def strip(die):
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        die = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    return die


def name(die):
    a = die.attributes.get("DW_AT_name")
    return a.value.decode() if a else None


def size_of(die):
    die = strip(die)
    if die is None:
        return 0
    if "DW_AT_byte_size" in die.attributes:
        return die.attributes["DW_AT_byte_size"].value
    if die.tag == "DW_TAG_array_type":
        return size_of(die.get_DIE_from_attribute("DW_AT_type")) * array_count(die)
    return 0


def array_count(die):
    n = 1
    for sub in die.iter_children():
        if sub.tag == "DW_TAG_subrange_type":
            if "DW_AT_count" in sub.attributes:
                n *= sub.attributes["DW_AT_count"].value
            elif "DW_AT_upper_bound" in sub.attributes:
                n *= sub.attributes["DW_AT_upper_bound"].value + 1
            else:
                n *= 0
    return n


def flatten(die, path, off, out):
    die = strip(die)
    if die is None:
        return
    t = die.tag
    if t in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        members = [m for m in die.iter_children() if m.tag == "DW_TAG_member"]
        if t == "DW_TAG_union_type":
            # union: ["path", off, size, "U", pad, [[view_name, [fields...]], ...]] - every view of the
            # same bytes; statediff reports which views agree (a wide-int view agreeing while a byte
            # view differs is the little-endian trap: written as an integer, read as bytes)
            views = []
            for i, m in enumerate(members):
                vf = []
                flatten(m.get_DIE_from_attribute("DW_AT_type"), "", 0, vf)
                views.append([name(m) or "anon%d" % i, vf])
            out.append([path, off, size_of(die), "U", False, views])
            return
        n_anon = 0
        for m in members:
            moff = m.attributes["DW_AT_data_member_location"].value if "DW_AT_data_member_location" in m.attributes else 0
            mname = name(m)
            if mname is None:  # anonymous struct/union: number them so 3DS/N64 paths pair up uniquely
                mname = "anon%d" % n_anon
                n_anon += 1
            if "DW_AT_bit_size" in m.attributes:
                # bitfield: [path, unit_off, unit_size, "b", pad, bit_pos, bit_size]; bit_pos counts in
                # declaration order from the unit start (LE: from the LSB, BE/MIPS: from the MSB)
                usz = size_of(m.get_DIE_from_attribute("DW_AT_type"))
                bs = m.attributes["DW_AT_bit_size"].value
                if "DW_AT_data_bit_offset" in m.attributes:
                    ab = m.attributes["DW_AT_data_bit_offset"].value
                else:
                    ab = moff * 8
                uoff = (ab // 8) // usz * usz
                out.append([(path + "." if path else "") + mname, off + uoff, usz, "b", False, ab - uoff * 8, bs])
                continue
            flatten(m.get_DIE_from_attribute("DW_AT_type"), path + "." + mname if path else mname, off + moff, out)
    elif t == "DW_TAG_array_type":
        elem = die.get_DIE_from_attribute("DW_AT_type")
        esz, n = size_of(elem), array_count(die)
        for i in range(min(n, MAX_EXPAND)):
            flatten(elem, "%s[%d]" % (path, i), off + i * esz, out)
    elif t == "DW_TAG_pointer_type":
        out.append([path, off, 4, "p", False])
    elif t == "DW_TAG_base_type":
        sz = die.attributes["DW_AT_byte_size"].value
        enc = die.attributes["DW_AT_encoding"].value  # 4 float, 5 signed, 6 signed char, 7 unsigned, 8 unsigned char, 2 bool
        kind = "f" if enc == 4 and sz == 4 else "d" if enc == 4 else "s" if enc in (5, 6) else "u"
        out.append([path, off, sz, kind, False])
    elif t == "DW_TAG_enumeration_type":
        out.append([path, off, die.attributes["DW_AT_byte_size"].value, "s", False])


def extract_obj(obj):
    layouts = {}
    with open(obj, "rb") as f:
        elf = ELFFile(f)
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag == "DW_TAG_variable" and (name(die) or "").startswith("probe_"):
                    tname = name(die)[len("probe_"):]
                    fields = []
                    ty = die.get_DIE_from_attribute("DW_AT_type")
                    flatten(ty, "", 0, fields)
                    for fl in fields:
                        fl[4] = bool(PAD_RE.search(fl[0]))  # unions: padding views are compared too
                    layouts[tname] = {"size": size_of(ty), "fields": fields}
    return layouts


def merge(f3, fn):
    """Attach the N64 size and offset (last two items) to each 3DS field, pairing by path; unions
    recurse per view."""
    by_path = {f[0]: f for f in fn}
    out = []
    for f in f3:
        g = by_path.get(f[0])
        if g is None:
            continue
        if f[3] == "U":
            nviews = dict((v[0], v[1]) for v in g[5])
            views = [[v[0], merge(v[1], nviews.get(v[0], []))] for v in f[5]]
            out.append(f[:5] + [views, g[2], g[1]])
        else:
            out.append(f + [g[2], g[1]])  # N64 size (IDO enums are 4 bytes), N64 offset last
    return out


def extract():
    build_probe(PROBE_OBJ, False)
    build_probe(PROBE_OBJ.replace(".o", "_n64.o"), True)
    l3, ln = extract_obj(PROBE_OBJ), extract_obj(PROBE_OBJ.replace(".o", "_n64.o"))
    for t in l3:
        l3[t]["n64_size"] = ln[t]["size"]
        l3[t]["fields"] = merge(l3[t]["fields"], ln[t]["fields"])
    return l3


def annotated_sizes():
    sizes = {}
    for root, _, files in os.walk(os.path.join(REPO, "include")):
        for fn in files:
            if fn.endswith(".h"):
                for m in re.finditer(r"\}\s*(\w+);\s*//\s*size\s*=\s*(0x[0-9A-Fa-f]+)", open(os.path.join(root, fn), errors="ignore").read()):
                    sizes[m.group(1)] = int(m.group(2), 16)
    return sizes


def main():
    layouts = extract()
    ann = annotated_sizes()
    ok = True
    for t, l in layouts.items():
        want = ann.get(t)
        verdict = "no annotation" if want is None else ("OK" if want == l["n64_size"] == l["size"] else "MISMATCH")
        if verdict == "MISMATCH" and want - l["size"] == 0x10 and l["size"] == l["n64_size"]:
            verdict = "OK (annotation includes Actor's DEBUG_FEATURES dbgPad[0x10]; retail has none)"
        ok &= verdict != "MISMATCH"
        print("%-20s 3DS size 0x%X  N64 (decomp annotation) %s  %s  (%d leaf fields)" %
              (t, l["size"], "0x%X" % want if want else "-", verdict, len(l["fields"])))
    json.dump(layouts, open(os.path.join(OUT_DIR, "layout.json"), "w"))
    print("wrote build/statediff/layout.json")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

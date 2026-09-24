#!/usr/bin/env python3
"""gbi_audit.py — static coverage audit: which N64 display-list opcodes does OoT emit, and which
does the port's Fast3D interpreter (port/src/gfx/gfx_pc.c) actually handle?

1. Scan every game source + extracted asset .c for gSP*/gDP*/gs* macro uses (with counts).
2. Expand each macro through include/ultra64/gbi.h (F3DEX2 branch) recursively to the opcode
   constants it emits.
3. Compare against the `case` labels of gfx_run_dl() in gfx_pc.c.
Output: every opcode OoT can emit, how many call sites reach it, and HANDLED / MISSING.
"""
import os, re, collections

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GBI = os.path.join(REPO, "include/ultra64/gbi.h")
GFXPC = os.path.join(REPO, "port/src/gfx/gfx_pc.c")
SCAN = [os.path.join(REPO, "src"), os.path.join(REPO, "extracted/ntsc-1.0/assets")]

OPCODES = """G_NOOP G_VTX G_MODIFYVTX G_CULLDL G_BRANCH_Z G_TRI1 G_TRI2 G_QUAD G_LINE3D G_SPECIAL_1
G_SPECIAL_2 G_SPECIAL_3 G_DMA_IO G_TEXTURE G_POPMTX G_GEOMETRYMODE G_MTX G_MOVEWORD G_MOVEMEM
G_LOAD_UCODE G_DL G_ENDDL G_SPNOOP G_RDPHALF_1 G_RDPHALF_2 G_SETOTHERMODE_L G_SETOTHERMODE_H
G_TEXRECT G_TEXRECTFLIP G_RDPLOADSYNC G_RDPPIPESYNC G_RDPTILESYNC G_RDPFULLSYNC G_SETKEYGB
G_SETKEYR G_SETCONVERT G_SETSCISSOR G_SETPRIMDEPTH G_RDPSETOTHERMODE G_LOADTLUT G_SETTILESIZE
G_LOADBLOCK G_LOADTILE G_SETTILE G_FILLRECT G_SETFILLCOLOR G_SETFOGCOLOR G_SETBLENDCOLOR
G_SETPRIMCOLOR G_SETENVCOLOR G_SETCOMBINE G_SETTIMG G_SETZIMG G_SETCIMG G_BG_1CYC G_BG_COPY
G_OBJ_RECTANGLE G_OBJ_SPRITE G_OBJ_MOVEMEM G_SELECT_DL G_OBJ_LOADTXTR G_OBJ_LDTX_SPRITE
G_OBJ_LDTX_RECT G_OBJ_LDTX_RECT_R G_OBJ_RECTANGLE_R G_RDPHALF_0""".split()
SYNC_NOOPS = {"G_RDPLOADSYNC", "G_RDPPIPESYNC", "G_RDPTILESYNC", "G_RDPFULLSYNC", "G_NOOP", "G_SPNOOP"}


def parse_gbi_defines():
    """name -> body text, taking the F3DEX_GBI_2 branch of #ifdef F3DEX_GBI_2 / #else blocks."""
    text = open(GBI).read().replace("\\\n", " ")
    defs = {}
    stack = []  # each: (kind, taking)
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b\s*(.*)", s)
        if m:
            kw, arg = m.group(1), m.group(2)
            if kw in ("ifdef", "ifndef", "if"):
                take = True
                if kw == "ifdef" and arg.strip() in ("F3D_OLD", "F3DEX_GBI", "F3DLP_GBI"):
                    take = False
                if kw == "ifndef" and arg.strip() == "F3DEX_GBI_2":
                    take = False
                if kw == "if" and "F3DEX_GBI_2" in arg and "!" not in arg:
                    take = True
                elif kw == "if" and ("defined(F3DEX_GBI)" in arg.replace(" ", "") and "F3DEX_GBI_2" not in arg):
                    take = False
                stack.append(take)
            elif kw == "else":
                if stack:
                    stack[-1] = not stack[-1]
            elif kw == "elif":
                if stack:
                    stack[-1] = "F3DEX_GBI_2" in arg
            elif kw == "endif":
                if stack:
                    stack.pop()
            continue
        if not all(stack):
            continue
        m = re.match(r"#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(\([^)]*\))?\s*(.*)", s)
        if m and m.group(1) not in defs:
            defs[m.group(1)] = m.group(3)
    return defs


def opcodes_of(name, defs, memo, depth=0):
    if name in memo:
        return memo[name]
    memo[name] = set()
    if depth > 40 or name not in defs:
        return memo[name]
    body = defs[name]
    ops = set(t for t in re.findall(r"\bG_[A-Z0-9_]+\b", body) if t in OPCODES)
    for ident in set(re.findall(r"\b(g[sS]?[PD][A-Za-z0-9_]*|gs[A-Za-z0-9_]+|g[A-Z][A-Za-z0-9_]*|_g[A-Za-z0-9_]+|__g[A-Za-z0-9_]+)\b", body)):
        if ident != name and ident in defs:
            ops |= opcodes_of(ident, defs, memo, depth + 1)
    memo[name] = ops
    return ops


def handled_cases():
    src = open(GFXPC).read()
    m = re.search(r"static void gfx_run_dl\(.*?\n}\n", src, re.S)
    body = m.group(0) if m else src
    return set(re.findall(r"case \(uint8_t\)(G_[A-Z0-9_]+)|case (G_[A-Z0-9_]+):", body)) and \
        set(a or b for a, b in re.findall(r"case \(uint8_t\)(G_[A-Z0-9_]+)|case (G_[A-Z0-9_]+):", body))


def main():
    defs = parse_gbi_defines()
    macro_use = collections.Counter()
    pat = re.compile(r"\b(gs?(?:SP|DP)[A-Za-z0-9_]*|gs?Move[A-Za-z0-9_]*|gs?Immp1|gs?Dma[0-9]p)\s*\(")
    for root in SCAN:
        for dp, _, files in os.walk(root):
            for f in files:
                if f.endswith((".c", ".h")):
                    try:
                        t = open(os.path.join(dp, f), errors="ignore").read()
                    except OSError:
                        continue
                    macro_use.update(pat.findall(t))
    memo = {}
    op_sites = collections.Counter()
    op_macros = collections.defaultdict(set)
    unresolved = []
    for mac, n in macro_use.items():
        ops = opcodes_of(mac, defs, memo)
        if not ops:
            unresolved.append((mac, n))
        for op in ops:
            op_sites[op] += n
            op_macros[op].add(mac)
    handled = handled_cases()
    print("%-20s %8s  %-8s  %s" % ("OPCODE", "SITES", "STATUS", "example macros"))
    for op, n in sorted(op_sites.items(), key=lambda kv: -kv[1]):
        st = "handled" if op in handled else ("noop-ok" if op in SYNC_NOOPS else "MISSING")
        print("%-20s %8d  %-8s  %s" % (op, n, st, ", ".join(sorted(op_macros[op])[:4])))
    print("\nhandled but never emitted:", sorted(handled - set(op_sites)))
    print("unresolved macros (no opcode found; check manually):",
          sorted(unresolved, key=lambda x: -x[1])[:25])


if __name__ == "__main__":
    main()

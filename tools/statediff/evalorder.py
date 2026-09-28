#!/usr/bin/env python3
"""evalorder.py - which order does each compiler call the Rand_* functions of one statement in?

C leaves the order of calls inside one expression unspecified; IDO (N64) and GCC (3DS) may draw
random numbers into different slots from the same RNG state. For every statement listed in
tools/statediff/eval_order_sites.txt this renames the statement's Rand calls to distinct stubs
(__RO<k>_<name>, same signatures), compiles the file with IDO (upstream reference worktree, the
reference build's exact flags) and with the port's GCC flags, and reads the stub call order from both
disassemblies. IDO's order depends only on the expression's shape, not on which function is called.

usage: evalorder.py [--only substring]
Prints per site: IDO order, GCC order, SAME/DIFF, and whether the calls are conditional (?:, &&, ||).
"""
import argparse, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import layout as L  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
WT = os.path.abspath(os.path.join(REPO, "..", "OOT-64-3DS-Port-n64ref"))
IDO_FLAGS = ("-c -DPLATFORM_N64=1 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 -DOOT_VERSION=NTSC_1_0 -DOOT_REVISION=0 "
             "-DOOT_REGION=REGION_US -DLIBULTRA_VERSION=LIBULTRA_VERSION_I -DLIBULTRA_PATCH=1 -DDEBUG_FEATURES=0 "
             "-DNDEBUG -DF3DEX_GBI_2 -G 0 -non_shared -fullwarn -verbose -Xcpluscomm -Iinclude -Iinclude/libc "
             "-Isrc -Ibuild/ntsc-1.0 -I. -Iextracted/ntsc-1.0 -Wab,-r4300_mul -woff 516,609,649,838,712,807 "
             "-mips2 -O2 -g3").split()
SIGS = {"Rand_ZeroOne": "f32 %s(void);", "Rand_ZeroFloat": "f32 %s(f32);", "Rand_CenteredFloat": "f32 %s(f32);",
        "Rand_S16Offset": "s16 %s(s16, s16);", "Rand_S16OffsetStride": "s16 %s(s16, s16, s16);",
        "Rand_Next": "u32 %s(void);"}
CALL = re.compile(r"\bRand_(ZeroOne|ZeroFloat|CenteredFloat|S16OffsetStride|S16Offset|Next)\b")
MIPS_OBJDUMP = "/opt/homebrew/bin/mips-linux-gnu-objdump"
ARM_OBJDUMP = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump"


def statement_span(lines, idx):
    """lines[idx] starts the statement; extend to the ';' closing it (paren depth 0)"""
    depth, j = 0, idx
    while j < len(lines):
        for ch in lines[j]:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == ";" and depth == 0:
                return idx, j
        j += 1
    return idx, idx


def instrument(src_text, lineno):
    """-> (text with the statement's Rand calls renamed, [names in source order], conditional?)"""
    lines = src_text.split("\n")
    a, b = statement_span(lines, lineno - 1)
    stmt = "\n".join(lines[a:b + 1])
    names = []

    def ren(m):
        k = len(names)
        names.append("Rand_" + m.group(1))
        return "__RO%d_Rand_%s" % (k, m.group(1))

    new = CALL.sub(ren, stmt)
    cond = bool(re.search(r"\?|&&|\|\|", stmt))
    decls = "".join(SIGS[n] % ("__RO%d_%s" % (k, n)) for k, n in enumerate(names))
    # declarations right before the function holding the statement: after the last top-level '}' or
    # include before line a
    ins = 0
    for i in range(a - 1, -1, -1):
        if lines[i].startswith("}") or lines[i].startswith("#include"):
            ins = i + 1
            break
    out = lines[:ins] + [decls] + lines[ins:a] + new.split("\n") + lines[b + 1:]
    return "\n".join(out), names, cond


def call_order(objdump, obj, pattern):
    dis = subprocess.run([objdump, "-dr", obj], capture_output=True, text=True).stdout
    return [int(m.group(1)) for m in re.finditer(pattern, dis)]


def ido_order(path, lineno):
    rel = os.path.relpath(path, REPO)
    src = open(os.path.join(WT, rel), encoding="latin-1").read()
    # the upstream line numbers match the port's for these sites unless the port edited the file: find
    # the same statement text by its first line
    port_line = open(path, encoding="latin-1").read().split("\n")[lineno - 1].strip()
    up_lines = src.split("\n")
    cands = [i + 1 for i, l in enumerate(up_lines) if l.strip() == port_line]
    if not cands:
        return None, "statement not found upstream"
    upl = min(cands, key=lambda n: abs(n - lineno))
    text, names, cond = instrument(src, upl)
    tmp = os.path.join(WT, os.path.dirname(rel), "__ro_tmp.c")
    obj = tempfile.mktemp(suffix=".o")
    open(tmp, "w", encoding="latin-1").write(text)
    try:
        r = subprocess.run(["./tools/preprocess.sh", "-v", "ntsc-1.0", "-i", "/opt/homebrew/opt/libiconv/bin/iconv", "--",
                            "tools/ido_recomp/macos/7.1/cc"] + IDO_FLAGS + ["-o", obj, os.path.relpath(tmp, WT)],
                           cwd=WT, capture_output=True, text=True)
        if r.returncode != 0:
            return None, "IDO failed: " + (r.stderr or r.stdout)[-300:]
        return call_order(MIPS_OBJDUMP, obj, r"R_MIPS_26\s+__RO(\d+)_"), (names, cond)
    finally:
        os.remove(tmp)
        if os.path.exists(obj):
            os.remove(obj)


def gcc_order(path, lineno):
    src = open(path, encoding="latin-1").read()
    text, names, cond = instrument(src, lineno)
    tmp = os.path.join(os.path.dirname(path), "__ro_tmp.c")
    obj = tempfile.mktemp(suffix=".o")
    open(tmp, "w", encoding="latin-1").write(text)
    try:
        r = subprocess.run([L.CC] + L.game_cflags() + ["-w", "-c", tmp, "-o", obj], cwd=REPO,
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None, "GCC failed: " + r.stderr[-300:]
        return call_order(ARM_OBJDUMP, obj, r"R_ARM_(?:CALL|JUMP24|PC24)\s+__RO(\d+)_"), (names, cond)
    finally:
        os.remove(tmp)
        if os.path.exists(obj):
            os.remove(obj)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    diff = 0
    for line in open(os.path.join(HERE, "eval_order_sites.txt")):
        if line.startswith("#") or not line.strip():
            continue
        loc, snippet = line.split(None, 1)
        if a.only and a.only not in loc:
            continue
        path, ln = loc.rsplit(":", 1)
        path, ln = os.path.join(REPO, path), int(ln)
        # the recorded line can be off: take the nearest line whose text starts like the listed statement
        key = re.sub(r"\s+", "", snippet)[:40]
        flines = open(path, encoding="latin-1").read().split("\n")
        cands = [i + 1 for i, l in enumerate(flines) if re.sub(r"\s+", "", l).startswith(key)]
        if cands:
            ln = min(cands, key=lambda n: abs(n - ln))
        io, info = ido_order(path, ln)
        go, ginfo = gcc_order(path, ln)
        if io is None or go is None:
            print("%-72s ERROR %s" % (loc, info if io is None else ginfo))
            continue
        names, cond = info
        same = io == go
        diff += not same
        print("%-72s IDO %-14s GCC %-14s %s%s" % (loc, io, go, "SAME" if same else "DIFF",
                                                  "  (conditional calls)" if cond else ""))
    print("%d sites differ" % diff)


if __name__ == "__main__":
    main()

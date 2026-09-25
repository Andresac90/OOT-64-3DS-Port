#!/usr/bin/env python3
"""layout_actors.py - full instance layouts for every actor type, for statediff's actor comparison.

For each ActorProfile in the decomp (src/overlays/actors/*, src/code/z_player_call.c) take its actor
id (include/tables/actor_table.h) and instance type (the profile's sizeof(Type)), compile the source
file with debug info twice (3DS flags as-is, and without __3DS__ for the N64 layout), and flatten the
type like layout.py does. Output: build/statediff/actors.json
  { "<id>": {"name": Type, "size": 3DS size, "n64_size": N64 size, "fields": [...]}, ... }
Files that fail to compile without __3DS__ fall back to the 3DS layout for N64 (reported).
"""
import json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from elftools.elf.elffile import ELFFile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout as L  # noqa: E402

TMP = os.path.join(L.OUT_DIR, "actor_objs")
PROFILE_RE = re.compile(r"ActorProfile\s+\w+_Profile\s*=\s*\{(.*?)\};", re.S)


def actor_ids():
    ids = {}
    for m in re.finditer(r"/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*DEFINE_ACTOR\w*\(\s*(?:\w+,\s*)?(ACTOR_\w+)",
                         open(os.path.join(L.REPO, "include/tables/actor_table.h")).read()):
        ids[m.group(2)] = int(m.group(1), 16)
    return ids


def profiles():
    found = []
    roots = [os.path.join(L.REPO, "src/overlays/actors"), os.path.join(L.REPO, "src/code/z_player_call.c")]
    files = [roots[1]] + [os.path.join(dp, f) for dp, _, fs in os.walk(roots[0]) for f in fs if f.endswith(".c")]
    for path in files:
        for m in PROFILE_RE.finditer(open(path, errors="ignore").read()):
            body = m.group(1)
            a, t = re.search(r"\b(ACTOR_\w+)", body), re.search(r"sizeof\((\w+)\)", body)
            if a and t:
                found.append((a.group(1), t.group(1), path))
    return found


def compile_layout(path, tname, n64, flags):
    res = _compile_layout(path, tname, n64, flags)
    if res is None:
        # the type is only used inside sizeof() there, so GCC emits no DWARF for it: wrap the file
        # and declare one instance to force it
        wrap = os.path.join(TMP, "%s_wrap.c" % tname)
        open(wrap, "w").write('#include "%s"\n%s statediff_probe_instance;\n' % (path, tname))
        res = _compile_layout(wrap, tname, n64, flags + ["-I" + os.path.dirname(path)])
    return res


def _compile_layout(path, tname, n64, flags):
    obj = os.path.join(TMP, "%s%s.o" % (tname, "_n64" if n64 else ""))
    fl = [f for f in flags if not (n64 and f == "-D__3DS__")] + (["-fno-short-enums"] if n64 else [])
    r = subprocess.run([L.CC] + fl + ["-g", "-O0", "-w", "-c", path, "-o", obj], cwd=L.REPO, capture_output=True)
    if r.returncode != 0:
        return None
    with open(obj, "rb") as f:
        for cu in ELFFile(f).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag == "DW_TAG_typedef" and L.name(die) == tname:
                    fields = []
                    L.flatten(die, "", 0, fields)
                    for fl_ in fields:
                        fl_[4] = bool(L.PAD_RE.search(fl_[0]))
                    return {"size": L.size_of(die), "fields": fields}
    return None


def main():
    os.makedirs(TMP, exist_ok=True)
    flags = L.game_cflags()
    ids = actor_ids()
    profs = [(ids[a], t, p) for a, t, p in profiles() if a in ids]
    only = sys.argv[1:]  # optional: type names to (re)build, merged into the existing actors.json
    if only:
        profs = [x for x in profs if x[1] in only]

    def work(item):
        aid, tname, path = item
        l3 = compile_layout(path, tname, False, flags)
        ln = compile_layout(path, tname, True, flags)
        return aid, tname, path, l3, ln

    path_json = os.path.join(L.OUT_DIR, "actors.json")
    out = json.load(open(path_json)) if only and os.path.exists(path_json) else {}
    fails, fallback = [], []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as ex:
        for aid, tname, path, l3, ln in ex.map(work, profs):
            if l3 is None:
                fails.append(tname)
                continue
            if ln is None:
                fallback.append(tname)
                ln = l3
            out[str(aid)] = {"name": tname, "size": l3["size"], "n64_size": ln["size"],
                             "fields": L.merge(l3["fields"], ln["fields"])}
    json.dump(out, open(os.path.join(L.OUT_DIR, "actors.json"), "w"))
    diffsize = [(v["name"], v["size"], v["n64_size"]) for v in out.values() if v["size"] != v["n64_size"]]
    print("actor layouts: %d types (%d failed to compile: %s; %d N64 fallback: %s)" %
          (len(out), len(fails), fails[:8], len(fallback), fallback[:8]))
    if diffsize:
        print("3DS/N64 size differences:", diffsize)
    print("wrote build/statediff/actors.json")


if __name__ == "__main__":
    main()

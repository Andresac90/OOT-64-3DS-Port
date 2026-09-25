#!/usr/bin/env python3
"""statediff.py - diff the 3DS port's game state against the real N64 game, field by field.

Both sides boot the same way (debug save -> Play, optional start entrance with child Link) and are
captured at the START of the same gameplay frame:
  N64 : pristine upstream decomp ROM (+ boot bypass only), run in ares, read over GDB at Play_Update
  3DS : the port built with GAME_EXTRA=-DPORT_STATEDUMP=<frame>, run in Azahar, dumped to the SD card
Structs are decoded with one layout (tools/statediff/layout.py, DWARF; sizes proven equal to N64).

usage: statediff.py [--entrance 0xB7] [--frame 60] [--pads] [--skip-3ds] [--skip-n64] [--max 200]
Differences are printed as  path: N64=<value>  3DS=<value>. Pointers are compared only for
NULL vs non-NULL; padding/unknown fields only with --pads.
"""
import argparse, json, os, re, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from gdbrsp import GdbRsp  # noqa: E402

OUT = os.path.join(REPO, "build/statediff")
SD = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/oot")
ARES = "/Applications/ares.app/Contents/MacOS/ares"
MIPS_NM = "/opt/homebrew/bin/mips-linux-gnu-nm"
ACTORCAT_MAX = 12


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=REPO, **kw)


def load_layout():
    path = os.path.join(OUT, "layout.json")
    if not os.path.exists(path):
        sh("%s tools/statediff/layout.py" % sys.executable, check=True)
    return json.load(open(path))


def load_actor_layouts():
    path = os.path.join(OUT, "actors.json")
    if not os.path.exists(path):
        sh("%s tools/statediff/layout_actors.py" % sys.executable, check=True)
    return json.load(open(path))


def field_off(layout, typ, path):
    for f in layout[typ]["fields"]:
        if f[0] == path:
            return f[1]
    raise KeyError("%s.%s" % (typ, path))


# ---------------------------------------------------------------- N64 (ares) capture
def n64_symbols(elf):
    syms = {}
    for line in subprocess.run([MIPS_NM, elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return syms


def n64_rand_state_addr(elf, syms):
    """sRandInt is static (IDO emits no symbol): decode Rand_Seed = lui at,HI ; jr ra ; sw a0,LO(at)."""
    a = syms["Rand_Seed"]
    dis = subprocess.run([MIPS_NM.replace("nm", "objdump"), "-d", "--start-address=0x%x" % a,
                          "--stop-address=0x%x" % (a + 12), elf], capture_output=True, text=True).stdout
    hi = int(re.search(r"lui\s+at,0x([0-9a-f]+)", dis).group(1), 16)
    lo = int(re.search(r"sw\s+a0,(-?\d+)\(at\)", dis).group(1))
    return ((hi << 16) + lo) & 0xFFFFFFFF


def capture_n64(layout, entrance, frame):
    tag = entrance or "default"
    rom, elf = os.path.join(OUT, "ref_%s.z64" % tag), os.path.join(OUT, "ref_%s.elf" % tag)
    if not os.path.exists(rom):
        sh("tools/statediff/make_ref.sh %s" % (entrance or ""), check=True)
    syms = n64_symbols(elf)
    bp = syms["Play_Update"]
    rng_addr = n64_rand_state_addr(elf, syms)
    rng_frames = {}
    sh("pkill -9 -f MacOS/ares")
    time.sleep(1)
    subprocess.Popen([ARES, "--system", "Nintendo 64", "--no-file-prompt", rom],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    g = GdbRsp(port=9123, connect_wait=30)
    try:
        g.set_break(bp)
        g.cont(timeout=120)
        off_frames = field_off(layout, "PlayState", "gameplayFrames")
        while True:
            play = g.read_regs()[4] & 0xFFFFFFFF
            n = struct.unpack(">I", g.read_mem(play + off_frames, 4))[0]
            rng_frames[n] = struct.unpack(">I", g.read_mem(rng_addr, 4))[0]
            if n == frame:
                break
            if n > frame:
                raise RuntimeError("N64 passed frame %d (at %d)" % (frame, n))
            g.cont_past(bp, timeout=60)
        blobs = {"play": g.read_mem(play, layout["PlayState"]["size"]),
                 "save": g.read_mem(syms["gSaveContext"], layout["SaveContext"]["size"])}
        lists = field_off(layout, "PlayState", "actorCtx.actorLists[0].head")
        stride = field_off(layout, "PlayState", "actorCtx.actorLists[1].head") - lists
        off_next = field_off(layout, "Actor", "next")
        off_id = field_off(layout, "Actor", "id")
        asz = layout["Actor"]["size"]
        actor_layouts = load_actor_layouts()
        actors = []
        for cat in range(ACTORCAT_MAX):
            a = struct.unpack(">I", blobs["play"][lists + cat * stride:lists + cat * stride + 4])[0]
            while a:
                head = g.read_mem(a, asz)
                aid = struct.unpack(">h", head[off_id:off_id + 2])[0]
                full = actor_layouts.get(str(aid), {}).get("n64_size", asz)  # whole instance
                data = head + g.read_mem(a + asz, full - asz) if full > asz else head
                actors.append((cat, data))
                a = struct.unpack(">I", head[off_next:off_next + 4])[0]
        blobs["actors"] = actors
        blobs["rng_frames"] = rng_frames
        blobs["zbuf"] = g.read_mem(syms["gZBuffer"], 320 * 240 * 2)  # N64 z-buffer (RDP-rendered)
    finally:
        g.detach()
        sh("pkill -9 -f MacOS/ares")
    return blobs


# ---------------------------------------------------------------- 3DS (Azahar) capture
def build_3ds(game_extra):
    # make doesn't track flags: drop the objects of every game file that reacts to these defines
    hooked = sh("grep -rlE 'PORT_STATEDUMP|PORT_START_ENTRANCE' src", capture_output=True, text=True).stdout.split()
    for src in hooked:
        o = os.path.join(REPO, "build/3ds", os.path.splitext(src)[0] + ".o")
        if os.path.exists(o):
            os.remove(o)
    r = sh("make -f Makefile.3ds cci GAME_EXTRA='%s' 2>&1 | grep -iE ' error|undefined reference' ; true" % game_extra,
           capture_output=True, text=True)
    if r.stdout.strip():
        raise RuntimeError("3DS build failed:\n" + r.stdout)


def capture_3ds(layout, entrance, frame):
    extra = "-DPORT_STATEDUMP=%d" % frame + (" -DPORT_START_ENTRANCE=%s" % entrance if entrance else "")
    build_3ds(extra)
    for f in ("sd_play.bin", "sd_save.bin", "sd_actors.bin", "sd_rng.bin", "sd_depth.bin", "boot.log"):
        if os.path.exists(os.path.join(SD, f)):
            os.remove(os.path.join(SD, f))
    sh("pkill -9 -f MacOS/azahar")
    time.sleep(1)
    app = sh("ls -d /Applications/*[Aa]zahar*/Azahar.app | head -1", capture_output=True, text=True).stdout.strip()
    sh("open -n '%s' --args '%s'" % (app, os.path.join(REPO, "build/3ds/oot.3ds")))
    deadline = time.time() + 180
    log = os.path.join(SD, "boot.log")
    try:
        while time.time() < deadline:
            time.sleep(2)
            if os.path.exists(log) and b"statedump: wrote" in open(log, "rb").read():
                time.sleep(1)
                break
        else:
            raise RuntimeError("3DS never reached gameplay frame %d (see %s)" % (frame, log))
    finally:
        sh("pkill -9 -f MacOS/azahar")
    blobs = {"play": open(os.path.join(SD, "sd_play.bin"), "rb").read(),
             "save": open(os.path.join(SD, "sd_save.bin"), "rb").read()}
    raw, actors, i = open(os.path.join(SD, "sd_actors.bin"), "rb").read(), [], 0
    while i + 8 <= len(raw):
        cat, size = struct.unpack("<II", raw[i:i + 8])
        actors.append((cat, raw[i + 8:i + 8 + size]))
        i += 8 + size
    blobs["actors"] = actors
    rng_path = os.path.join(SD, "sd_rng.bin")
    blobs["rng_trace"] = open(rng_path, "rb").read() if os.path.exists(rng_path) else b""
    depth_path = os.path.join(SD, "sd_depth.bin")
    blobs["depth"] = open(depth_path, "rb").read() if os.path.exists(depth_path) else b""
    build_3ds("")  # leave the normal ROM in build/3ds
    return blobs


# ---------------------------------------------------------------- decode + diff
def value(buf, f, big):
    path, off, size, kind = f[:4]
    b = buf[off:off + size]
    if len(b) < size:
        return None
    end = ">" if big else "<"
    if kind == "b":
        unit = int.from_bytes(b, "big" if big else "little")
        pos, bits = f[5], f[6]
        shift = size * 8 - pos - bits if big else pos
        return (unit >> shift) & ((1 << bits) - 1)
    if kind == "f":
        return struct.unpack(end + "f", b)[0]
    if kind == "d":
        return struct.unpack(end + "d", b)[0]
    v = int.from_bytes(b, "big" if big else "little")
    if kind == "s" and v >= 1 << (size * 8 - 1):
        v -= 1 << (size * 8)
    if kind == "p":
        return "NULL" if v == 0 else "ptr"
    return v


def same(a, b, kind):
    if kind in ("f", "d"):
        if a != a and b != b:
            return True
        return abs(a - b) <= 1e-3 + 1e-4 * max(abs(a), abs(b))
    return a == b


def diff_fields(fields, n64, ds, base, prefix, pads, out):
    for f in fields:
        if f[4] and not pads:
            continue
        if f[3] == "U":
            diff_union(f, n64, ds, base, prefix, pads, out)
            continue
        g3 = [f[0], base[1] + f[1]] + f[2:]
        gn = [f[0], base[0] + f[-1]] + f[2:]
        a, b = value(n64, gn, True), value(ds, g3, False)
        if not same(a, b, f[3]):
            out.append((prefix + f[0], a, b))


def diff_union(f, n64, ds, base, prefix, pads, out):
    path, views = f[0], f[5]
    off = (base[0] + f[6], base[1] + f[1])  # (N64 offset, 3DS offset)
    results = []
    for vname, vfields in views:
        vout = []
        diff_fields(vfields, n64, ds, off, "", True, vout)
        results.append((vname, vout))
    agree = [v for v, o in results if not o]
    if len(agree) == len(results):
        return
    if not agree:  # no view agrees: a real value difference; report it through the first view
        for p, a, b in results[0][1]:
            out.append((prefix + path + "." + results[0][0] + p, a, b))
        return
    # some views agree, some don't: same bytes only under a subset of interpretations
    dis = [v for v, o in results if o]
    first = next(o for v, o in results if o)[0]
    out.append((prefix + path + " [UNION views agree: %s | differ: %s]" % (",".join(agree), ",".join(dis)),
                "%s=%s" % (first[0].lstrip("."), fmt(first[1])), fmt(first[2])))


def diff_struct(layout, typ, n64, ds, prefix, pads, out):
    diff_fields(layout[typ]["fields"], n64, ds, (0, 0), prefix, pads, out)


def fmt(v):
    return "%.4g" % v if isinstance(v, float) else (hex(v) if isinstance(v, int) and abs(v) > 9 else str(v))


def read_opt(path):
    return open(path, "rb").read() if os.path.exists(path) else b""


def rng_frames_3ds(trace):
    frames, i = {}, 0
    while i + 16 <= len(trace):
        k, v = struct.unpack("<II", trace[i:i + 8])
        if k == 0:
            frames[v] = struct.unpack("<II", trace[i + 8:i + 16])[1]
            i += 16
        else:
            i += 8
    return frames


def rng_report(n64, ds):
    """First gameplay frame whose starting RNG state differs (the divergence happened in the frame before)."""
    a, b = n64.get("rng_frames") or {}, rng_frames_3ds(ds.get("rng_trace") or b"")
    common = sorted(set(a) & set(b))
    if not common:
        return
    bad = [f for f in common if a[f] != b[f]]
    if bad:
        print("RNG: diverges before frame %d (N64 %08x, 3DS %08x); last equal frame %s -> run rngtrace.py --frame %d" %
              (bad[0], a[bad[0]], b[bad[0]], max([f for f in common if f < bad[0]], default="none"), bad[0] - 1))
    else:
        print("RNG: identical at the start of all %d frames compared" % len(common))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entrance", default="")
    ap.add_argument("--frame", type=int, default=60)
    ap.add_argument("--pads", action="store_true")
    ap.add_argument("--skip-n64", action="store_true", help="reuse build/statediff/n64_*.bin")
    ap.add_argument("--skip-3ds", action="store_true", help="reuse build/statediff/3ds_*.bin")
    ap.add_argument("--max", type=int, default=200)
    ap.add_argument("--all", action="store_true", help="also show allow-listed differences (allow.txt)")
    args = ap.parse_args()
    layout = load_layout()
    actor_layouts = load_actor_layouts()
    os.makedirs(OUT, exist_ok=True)

    def cache(side, blobs=None):
        p = os.path.join(OUT, "%s_%s_f%d.json" % (side, args.entrance or "default", args.frame))
        if blobs is not None:
            json.dump({"play": blobs["play"].hex(), "save": blobs["save"].hex(),
                       "actors": [[c, d.hex()] for c, d in blobs["actors"]],
                       "rng_frames": blobs.get("rng_frames", {}),
                       "rng_trace": blobs.get("rng_trace", b"").hex()}, open(p, "w"))
            for key in ("zbuf", "depth"):  # large raw buffers live next to the json
                if blobs.get(key):
                    open(p.replace(".json", "_%s.bin" % key), "wb").write(blobs[key])
            return blobs
        j = json.load(open(p))
        return {"play": bytes.fromhex(j["play"]), "save": bytes.fromhex(j["save"]),
                "actors": [(c, bytes.fromhex(d)) for c, d in j["actors"]],
                "rng_frames": {int(k): v for k, v in j.get("rng_frames", {}).items()},
                "rng_trace": bytes.fromhex(j.get("rng_trace", "")),
                "zbuf": read_opt(p.replace(".json", "_zbuf.bin")),
                "depth": read_opt(p.replace(".json", "_depth.bin"))}

    n64 = cache("n64") if args.skip_n64 else cache("n64", capture_n64(layout, args.entrance, args.frame))
    ds = cache("3ds") if args.skip_3ds else cache("3ds", capture_3ds(layout, args.entrance, args.frame))

    out = []
    diff_struct(layout, "PlayState", n64["play"], ds["play"], "play.", args.pads, out)
    diff_struct(layout, "SaveContext", n64["save"], ds["save"], "save.", args.pads, out)
    # actors: pair by (category, actor id, index among same id in that category)
    off_id = field_off(layout, "Actor", "id")
    def keyed(actors, big):
        seen, res = {}, {}
        for cat, d in actors:
            aid = struct.unpack((">" if big else "<") + "h", d[off_id:off_id + 2])[0]
            k = (cat, aid)
            seen[k] = seen.get(k, 0) + 1
            res[(cat, aid, seen[k] - 1)] = d
        return res
    an, ad = keyed(n64["actors"], True), keyed(ds["actors"], False)
    for k in sorted(set(an) | set(ad)):
        name = "actor[cat%d id0x%03X #%d]." % k
        if k not in ad:
            out.append((name + "(missing on 3DS)", "present", "-"))
        elif k not in an:
            out.append((name + "(extra on 3DS)", "-", "present"))
        else:
            al = actor_layouts.get(str(k[1]))
            if al is not None and len(an[k]) >= al["n64_size"] and len(ad[k]) >= al["size"]:
                name = "actor[cat%d %s #%d]." % (k[0], al["name"], k[2])
                diff_fields(al["fields"], an[k], ad[k], (0, 0), name, args.pads, out)
            else:
                diff_struct(layout, "Actor", an[k], ad[k], name, args.pads, out)

    allow = []
    for line in open(os.path.join(HERE, "allow.txt")):
        rx = line.split("  #")[0].split(" #")[0].strip()
        if rx and not line.startswith("#"):
            allow.append(re.compile(rx))
    allowed = [o for o in out if any(r.search(o[0]) for r in allow)]
    if not args.all:
        out = [o for o in out if not any(r.search(o[0]) for r in allow)]

    rng_report(n64, ds)
    print("statediff entrance=%s frame=%d: %d actors N64 / %d actors 3DS, %d differing fields (+%d allow-listed, --all shows them)" %
          (args.entrance or "default", args.frame, len(an), len(ad), len(out), len(allowed) if not args.all else 0))
    for path, a, b in out[:args.max]:
        print("  %-60s N64=%-14s 3DS=%s" % (path, fmt(a), fmt(b)))
    if len(out) > args.max:
        print("  ... %d more (--max)" % (len(out) - args.max))


if __name__ == "__main__":
    main()

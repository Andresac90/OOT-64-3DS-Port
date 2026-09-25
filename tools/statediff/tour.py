#!/usr/bin/env python3
"""tour.py - compare N64 vs 3DS game state in EVERY scene without playing to them.

One boot per side. The shared hook (statediff_input.h) spends --frames gameplay frames in each scene,
captures (N64: GDB breakpoint on StateDiff_Captured; 3DS: SD dump sdmc:/3ds/oot/tour/sd_*_<i>.bin) and
jumps to the next entrance with an instant transition, the same mechanism loading zones use.
A scene that hangs or crashes shows up as the capture index where one side stops.

usage: tour.py [--age adult|child] [--frames 40] [--scenes SCENE_A,SCENE_B] [--skip-n64] [--skip-3ds]
"""
import argparse, glob, json, os, re, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import statediff as SD  # noqa: E402
import rngtrace as RT  # noqa: E402
from gdbrsp import GdbRsp  # noqa: E402

AGES = {"adult": 0, "child": 1}  # LINK_AGE_ADULT / LINK_AGE_CHILD
TOUR_SD = os.path.join(SD.SD, "tour")


# (scene, age) combinations where the ORIGINAL game faults when entered this way with the debug save:
# the tour skips them (the port need not reproduce a crash). Found with the tools/statediff tour.
N64_CRASHES = {
    # none known: the hot-room crashes (FP exception drawing Navi's warning with the debug save's name)
    # are avoided on both sides by statediff_input.h; the tour also recovers from any new N64 stop
}


def retail_scene_entrances(age="child"):
    """One entrance per scene, in entrance-table order, valid for Link's age: Play_Init loads
    gEntranceTable[entrance + sceneLayer] (child day layer 0, adult day layer 2), so the row at that
    offset must belong to the same scene."""
    debug = set(re.findall(r"SCENE_\w+", open(os.path.join(SD.REPO, "include/tables/scene_table.h")).read()
                           .split("#if DEBUG_ASSETS")[1].split("#endif")[0]))
    rows = {int(m.group(1), 16): (m.group(2), m.group(3)) for m in re.finditer(
        r"/\* 0x([0-9A-F]+) \*/ DEFINE_ENTRANCE\((ENTR_\w+), (SCENE_\w+), \d+,",
        open(os.path.join(SD.REPO, "include/tables/entrance_table.h")).read())}
    layer = 2 if age == "adult" else 0
    seen = {}
    for idx in sorted(rows):
        if idx + layer not in rows:
            continue
        name = rows[idx][0]
        scene = rows[idx + layer][1]  # the scene this entrance actually loads at this age
        # SCENE_UNUSED_*: never loaded by the retail game; entering one hangs the real N64 too
        if scene in seen or scene in debug or scene.startswith("SCENE_UNUSED_") or (scene, age) in N64_CRASHES:
            continue
        seen[scene] = (idx, name)
    return [(scene, idx, name) for scene, (idx, name) in seen.items()]


def n64_fault(g, syms, sym):
    """libultra's faulted thread, if any: (exception code, pc symbol) - the original game crashed."""
    ft = struct.unpack(">I", g.read_mem(syms["__osFaultedThread"], 4))[0]
    if not ft:
        return None
    sr, pc, cause, badv = struct.unpack(">IIII", g.read_mem(ft + 0x20 + 0xF8, 16))
    return (cause >> 2) & 31, sym(pc)


def capture_n64(tour, age, layout, tag):
    """Capture every scene on the N64 reference. If the ORIGINAL game crashes or hangs in a scene, record
    it and relaunch ares resuming at the next scene (break at Play_Init, set the capture index and
    entrance in memory), so one bad scene doesn't end the tour."""
    rom, elf = SD.ref_rom("0x%03X" % tour[0][1], "idle", tag=tag, age=str(age), tour=True)
    syms = SD.n64_symbols(elf)
    sym = RT.Symbolizer(SD.MIPS_NM, elf)
    anchor, idx_addr = syms["StateDiff_Captured"], syms["gStateDiffCaptureIdx"]
    entr_off = SD.field_off(layout, "SaveContext", "save.entranceIndex")
    caps, crashes, start = {}, {}, 0
    while start < len(tour):
        SD.sh("pkill -9 -f MacOS/ares")
        time.sleep(1)
        subprocess.Popen([SD.ARES, "--system", "Nintendo 64", "--no-file-prompt", rom],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        g = GdbRsp(port=9123, connect_wait=30)
        i = start - 1
        try:
            if start > 0:  # resume: first Play_Init loads tour[start] and the next capture is #start
                g.set_break(syms["Play_Init"])
                g.cont_until(syms["Play_Init"], timeout=180)
                g.clear_break(syms["Play_Init"])
                g.write_mem(idx_addr, struct.pack(">i", start))
                g.write_mem(syms["gSaveContext"] + entr_off, struct.pack(">i", tour[start][1]))
            g.set_break(anchor)
            g.cont_until(anchor, timeout=180)
            while True:
                play = g.read_regs()[4] & 0xFFFFFFFF
                i = struct.unpack(">i", g.read_mem(idx_addr, 4))[0]
                caps[i] = SD.read_n64_state(g, syms, layout, play)
                caps[i]["zbuf"] = g.read_mem(syms["gZBuffer"], 320 * 240 * 2)
                # framebuffers (RGBA5551, 320x240): the one the VI displays and both SysCfb buffers
                vi = struct.unpack(">I", g.read_mem(syms["__osViCurr"], 4))[0]
                disp = struct.unpack(">I", g.read_mem(vi + 4, 4))[0] | 0x80000000
                cfb = struct.unpack(">II", g.read_mem(syms["sSysCfbFbPtr"], 8))
                caps[i]["fbdisp"] = g.read_mem(disp, 320 * 240 * 2)
                caps[i]["fb0"] = g.read_mem(cfb[0] | 0x80000000, 320 * 240 * 2)
                caps[i]["fb1"] = g.read_mem(cfb[1] | 0x80000000, 320 * 240 * 2)
                forced = struct.unpack(">4I", g.read_mem(syms["gStateDiffForcedMask"], 16))
                caps[i]["forced"] = bool(forced[i >> 5] & (1 << (i & 31)))
                print("  N64 capture %d/%d  %s" % (i + 1, len(tour), tour[i][0]), flush=True)
                if i + 1 >= len(tour):
                    start = len(tour)
                    break
                g.cont_until(anchor, timeout=180, from_addr=anchor)
        except (TimeoutError, OSError, ConnectionError):
            bad = i + 1
            try:
                g.interrupt()
                f = n64_fault(g, syms, sym)
            except Exception:
                f = None
            crashes[bad] = ("fault: exception %d in %s" % f) if f else "hang (no fault)"
            print("  N64: original game stopped in %s (%s); resuming at the next scene" %
                  (tour[bad][0] if bad < len(tour) else "?", crashes[bad]), flush=True)
            start = bad + 1
        finally:
            try:
                g.detach()
            except Exception:
                pass
            SD.sh("pkill -9 -f MacOS/ares")
    json.dump({str(k): v for k, v in crashes.items()}, open(os.path.join(SD.OUT, "n64_%s_crashes.json" % tag), "w"))
    return caps


def capture_3ds(tour, age):
    extra = SD.game_extra("0x%03X" % tour[0][1], -1, "idle", age=str(age), tour=True)
    SD.build_3ds(extra)
    for f in glob.glob(os.path.join(TOUR_SD, "*.bin")) + [os.path.join(SD.SD, "boot.log")]:
        if os.path.exists(f):
            os.remove(f)
    SD.sh("pkill -9 -f MacOS/azahar")
    time.sleep(1)
    app = SD.sh("ls -d /Applications/*[Aa]zahar*/Azahar.app | head -1", capture_output=True, text=True).stdout.strip()
    SD.sh("open -n '%s' --args '%s'" % (app, os.path.join(SD.REPO, "build/3ds/oot.3ds")))
    last, last_t = -1, time.time()
    try:
        while time.time() - last_t < 120:
            time.sleep(3)
            done = len(glob.glob(os.path.join(TOUR_SD, "sd_actors_*.bin")))
            if done - 1 > last:
                last, last_t = done - 1, time.time()
                print("  3DS captures: %d/%d" % (done, len(tour)), flush=True)
            if done >= len(tour):
                time.sleep(3)
                break
    finally:
        SD.sh("pkill -9 -f MacOS/azahar")
    caps = {}
    for i in range(len(tour)):
        paths = [os.path.join(TOUR_SD, "sd_%s_%d.bin" % (k, i)) for k in ("play", "save", "actors")]
        if not all(os.path.exists(p) for p in paths):
            break
        raw, actors, j = open(paths[2], "rb").read(), [], 0
        while j + 8 <= len(raw):
            cat, size = struct.unpack("<II", raw[j:j + 8])
            actors.append((cat, raw[j + 8:j + 8 + size]))
            j += 8 + size
        gp = os.path.join(TOUR_SD, "sd_glob_%d.bin" % i)
        caps[i] = {"play": open(paths[0], "rb").read(), "save": open(paths[1], "rb").read(), "actors": actors,
                   "glob": open(gp, "rb").read() if os.path.exists(gp) else b""}
        for k in ("depth", "color0", "color1"):
            dp = os.path.join(TOUR_SD, "sd_%s_%d.bin" % (k, i))
            caps[i][k] = open(dp, "rb").read() if os.path.exists(dp) else b""
    SD.build_3ds("")
    return caps


def save_caps(path, caps):
    for i, c in caps.items():  # depth buffers next to the json (large, raw)
        for k in ("zbuf", "depth", "fbdisp", "fb0", "fb1", "color0", "color1"):
            if c.get(k):
                open(path.replace(".json", "_%s_%d.bin" % (k, i)), "wb").write(c[k])
    json.dump({str(i): {"play": c["play"].hex(), "save": c["save"].hex(), "forced": c.get("forced", False),
                        "glob": c.get("glob", b"").hex(),
                        "actors": [[a, d.hex()] for a, d in c["actors"]]} for i, c in caps.items()}, open(path, "w"))


def load_caps(path):
    caps = _load_caps(path)
    for i, c in caps.items():
        for k in ("zbuf", "depth", "fbdisp", "fb0", "fb1", "color0", "color1"):
            f = path.replace(".json", "_%s_%d.bin" % (k, i))
            c[k] = open(f, "rb").read() if os.path.exists(f) else b""
    return caps


def _load_caps(path):
    return {int(i): {"play": bytes.fromhex(c["play"]), "save": bytes.fromhex(c["save"]), "forced": c.get("forced", False),
                     "glob": bytes.fromhex(c.get("glob", "")),
                     "actors": [(a, bytes.fromhex(d)) for a, d in c["actors"]]} for i, c in json.load(open(path)).items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--age", default="child", choices=sorted(AGES))
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--scenes", default="", help="comma-separated SCENE_ names (default: all retail scenes)")
    ap.add_argument("--skip-n64", action="store_true")
    ap.add_argument("--skip-3ds", action="store_true")
    ap.add_argument("--show", type=int, default=4, help="differences shown per scene")
    args = ap.parse_args()
    tour = retail_scene_entrances(args.age)
    if args.scenes:
        want = args.scenes.split(",")
        tour = [t for t in tour if t[0] in want]
    age = AGES[args.age]
    tag = "tour_%s_%d_%d" % (args.age, args.frames, len(tour))
    SD.write_script_header("idle")
    SD.write_tour_header([t[1] for t in tour], args.frames)
    layout, actor_layouts = SD.load_layout(), SD.load_actor_layouts()
    pn, p3 = os.path.join(SD.OUT, "n64_%s.json" % tag), os.path.join(SD.OUT, "3ds_%s.json" % tag)
    n64 = load_caps(pn) if args.skip_n64 else capture_n64(tour, age, layout, tag)
    if not args.skip_n64:
        save_caps(pn, n64)
    ds = load_caps(p3) if args.skip_3ds else capture_3ds(tour, age)
    if not args.skip_3ds:
        save_caps(p3, ds)

    clean = 0
    print("\nscene tour (%s Link, %d frames per scene): %d scenes, N64 reached %d, 3DS reached %d" %
          (args.age, args.frames, len(tour), len(n64), len(ds)))
    cp = os.path.join(SD.OUT, "n64_%s_crashes.json" % tag)
    crashes = {int(k): v for k, v in json.load(open(cp)).items()} if os.path.exists(cp) else {}
    for i, (scene, entr, name) in enumerate(tour):
        if i in crashes and i not in n64:
            print("  %-3d %-28s ORIGINAL GAME STOPPED on N64 (%s)%s" % (i, scene, crashes[i],
                  "; 3DS ran it" if i in ds else ""))
            continue
        if i not in n64 or i not in ds:
            print("  %-3d %-28s %s" % (i, scene, "NOT REACHED on " + " and ".join(
                s for s, c in (("N64", n64), ("3DS", ds)) if i not in c)))
            continue
        out, allowed, an, ad = SD.compare(n64[i], ds[i], layout, actor_layouts)
        clean += not out
        note = "  (watchdog capture: scene never settled into normal play, on N64 too)" if n64[i].get("forced") else ""
        print("  %-3d %-28s actors %3d/%-3d  %s%s" % (i, scene, len(an), len(ad),
                                                     "OK" if not out else "%d DIFFERENCES" % len(out), note))
        for path, a, b in out[:args.show]:
            print("        %-58s N64=%-12s 3DS=%s" % (path[:58], SD.fmt(a), SD.fmt(b)))
    print("%d/%d scenes identical" % (clean, len(tour)))


if __name__ == "__main__":
    main()

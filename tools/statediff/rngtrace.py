#!/usr/bin/env python3
"""rngtrace.py - find the exact call where the 3DS port's RNG sequence leaves the N64's.

statediff.py prints "RNG: diverges before frame F+1 ... run rngtrace.py --frame F". This replays that
gameplay frame on the N64 reference (ares + GDB breakpoints on every global-RNG entry point, overlay
return addresses mapped back to ELF symbols) and compares the ordered list of (RNG function, calling
function) with the 3DS trace recorded by the same statediff capture (sd_rng.bin).

usage: rngtrace.py --entrance 0x102 --frame 15 [--dump-frame 100] [--script idle]
"""
import argparse, bisect, json, os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import statediff as SD  # noqa: E402
from gdbrsp import GdbRsp  # noqa: E402

ARM_NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
RNG_FUNCS = ["Rand_Next", "Rand_ZeroOne", "Rand_ZeroFloat", "Rand_CenteredFloat", "Rand_S16Offset",
             "Rand_S16OffsetStride"]  # trace kinds 1..6 in port/src/statedump.c
OVERLAY_TABLES = [("gActorOverlayTable", 0x20), ("gEffectSsOverlayTable", 0x1C)]


class Symbolizer:
    def __init__(self, nm, elf):
        syms = []
        for line in subprocess.run([nm, "-n", elf], capture_output=True, text=True).stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in "Tt" and not p[2].startswith("$"):
                syms.append((int(p[0], 16), p[2]))
        self.addrs = [a for a, _ in syms]
        self.names = [n for _, n in syms]

    def __call__(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        return self.names[i] if i >= 0 else "?%08x" % addr


def n64_trace(entrance, frame, script, age=""):
    rom, elf = SD.ref_rom(entrance, script, tag="%s_%s%s" % (entrance or "default", script, "_age" + age if age else ""),
                          age=age)
    syms = SD.n64_symbols(elf)
    sizes = {}
    for line in subprocess.run([SD.MIPS_NM, "-S", elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 4:
            sizes[p[3]] = int(p[1], 16)
    sym = Symbolizer(SD.MIPS_NM, elf)
    layout = SD.load_layout()
    off_frames = SD.field_off(layout, "PlayState", "gameplayFrames")
    play_update = syms["StateDiff_Sync"]  # frame anchor, see statediff_input.h
    rng = {syms[n]: k + 1 for k, n in enumerate(RNG_FUNCS)}

    SD.sh("pkill -9 -f MacOS/ares")
    time.sleep(1)
    subprocess.Popen([SD.ARES, "--system", "Nintendo 64", "--no-file-prompt", rom],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    g = GdbRsp(port=9123, connect_wait=30)
    calls = []
    try:
        g.set_break(play_update)
        g.cont(timeout=120)
        while True:
            play = g.read_regs()[4] & 0xFFFFFFFF
            n = struct.unpack(">I", g.read_mem(play + off_frames, 4))[0]
            if n == frame:
                break
            g.cont_past(play_update, timeout=60)
        for a in rng:
            g.set_break(a)
        pc = play_update
        overlays = {}

        def read_overlays():
            for tname, esz in OVERLAY_TABLES:
                raw = g.read_mem(syms[tname], sizes[tname])
                for i in range(0, len(raw) - esz + 1, esz):
                    vs, ve, ld = struct.unpack(">III", raw[i + 8:i + 20])
                    if ld and vs:
                        overlays[ld] = (ld + (ve - vs), vs)

        read_overlays()
        while True:
            g.cont_past(pc, timeout=60)
            regs = g.read_regs()
            pc = regs[37] & 0xFFFFFFFF
            if pc == play_update:
                break
            if pc in rng:
                calls.append((rng[pc], regs[31] & 0xFFFFFFFF))
        read_overlays()
    finally:
        g.detach()
        SD.sh("pkill -9 -f MacOS/ares")

    def to_vram(addr):
        for ld, (end, vs) in overlays.items():
            if ld <= addr < end:
                return addr - ld + vs
        return addr
    return [(k, sym(to_vram(ra))) for k, ra in calls]


def ds_trace(entrance, frame, dump_frame, script, age_name=""):
    j = json.load(open(os.path.join(SD.OUT, "3ds_%s_%s%s_f%d.json" % (entrance or "default", script,
                                                                     "_" + age_name if age_name else "", dump_frame))))
    raw = bytes.fromhex(j["rng_trace"])
    sym = Symbolizer(ARM_NM, os.path.join(SD.REPO, "build/3ds/oot.elf"))
    out, cur, i = [], None, 0
    while i + 8 <= len(raw):
        k, v = struct.unpack("<II", raw[i:i + 8])
        if k == 0:
            cur = v
            i += 16
            continue
        if cur == frame:
            out.append((k, sym(v)))
        i += 8
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entrance", default="")
    ap.add_argument("--frame", type=int, required=True, help="gameplay frame to trace (the last equal one)")
    ap.add_argument("--dump-frame", type=int, default=100, help="--frame of the statediff capture to use")
    ap.add_argument("--script", default="idle")
    ap.add_argument("--age", default="", choices=["", "adult", "child"])
    args = ap.parse_args()
    # the 3DS trace symbols come from build/3ds/oot.elf: it must be the comparison build's code layout;
    # statediff rebuilds the normal ROM afterwards, which only differs in the hooked files, so rebuild
    # the comparison build here to symbolize against the exact binary that produced the trace
    age = SD.AGE_IDS.get(args.age, "")
    SD.build_3ds(SD.game_extra(args.entrance, args.dump_frame, args.script, age=age))
    ds = ds_trace(args.entrance, args.frame, args.dump_frame, args.script, args.age)
    SD.build_3ds("")
    n64 = n64_trace(args.entrance, args.frame, args.script, age)
    print("frame %d: %d RNG calls on N64, %d on 3DS" % (args.frame, len(n64), len(ds)))
    i = 0
    while i < min(len(n64), len(ds)) and n64[i] == ds[i]:
        i += 1
    lo = max(0, i - 6)
    for j in range(lo, min(max(len(n64), len(ds)), i + 8)):
        a = "%s <- %s" % (RNG_FUNCS[n64[j][0] - 1], n64[j][1]) if j < len(n64) else "-"
        b = "%s <- %s" % (RNG_FUNCS[ds[j][0] - 1], ds[j][1]) if j < len(ds) else "-"
        print("%s %4d  N64 %-55s 3DS %s" % (">>" if j == i else "  ", j, a, b))
    if i == len(n64) == len(ds):
        print("sequences identical")
    else:
        print("first difference at call #%d" % i)


if __name__ == "__main__":
    main()

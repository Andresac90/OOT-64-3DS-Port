#!/usr/bin/env python3
"""pcprof.py - sampling CPU profiler for the 3DS build, through Azahar's GDB stub (2026-10-03).

Boots build/3ds/oot.3ds in Azahar with the GDB stub enabled, lets the title attract demo run, then
repeatedly interrupts the emulated CPU, reads the program counter and link register, and continues.
Samples are mapped to functions with the ELF's symbol table. Azahar's emulated time follows the
instruction count, so the shares are instruction shares: where the CPU work is, not cache effects
(hardware is 1.5-3x slower, roughly uniformly; see docs/3ds-native-speed-research.md).

usage: tools/pcprof.py [--samples 3000] [--warmup 45] [--interval 0.004] [--top 40]
                       [--setting name=value ...]
The settings are written to the emulator's sdmc:/3ds/oot/settings.txt for the run (restored after);
e.g. --setting fps60=0 --setting frameskip=1 for the Old 3DS path (no in-between frames, coarse splits).
The emulator's qt-config.ini is restored afterwards (the stub makes the boot wait for a debugger).
"""
import argparse, bisect, collections, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "statediff"))
from gdbrsp import GdbRsp  # noqa: E402

AZ_CFG = os.path.expanduser("~/Library/Application Support/Azahar/config/qt-config.ini")
SD = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/oot")
NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
ELF = os.path.join(REPO, "build/3ds/oot.elf")
ROM = os.path.join(REPO, "build/3ds/oot.3ds")
PORT = 24689


def symbols():
    out = subprocess.run([NM, "-n", "-S", "--defined-only", ELF], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 3 and f[-2] in "tTwW":
            addrs.append(int(f[0], 16))
            names.append(f[-1])
    return addrs, names


def lookup(addrs, names, pc):
    i = bisect.bisect_right(addrs, pc) - 1
    return names[i] if i >= 0 else "?"


def set_stub(cfg_text, on):
    lines = []
    for line in cfg_text.splitlines():
        if line.startswith("use_gdbstub="):
            line = "use_gdbstub=%s" % ("true" if on else "false")
        elif line.startswith("use_gdbstub\\default="):
            line = "use_gdbstub\\default=%s" % ("false" if on else "true")
        lines.append(line)
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=3000)
    ap.add_argument("--warmup", type=float, default=45.0, help="seconds after boot before sampling")
    ap.add_argument("--interval", type=float, default=0.05)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--setting", action="append", default=[])
    ap.add_argument("--lines", default="", help="comma-separated functions: also list their hottest source lines "
                    "(build with PORT_EXTRA=-g for line info)")
    args = ap.parse_args()
    settings = args.setting

    cfg = open(AZ_CFG).read()
    st_path = os.path.join(SD, "settings.txt")
    st_old = open(st_path).read() if os.path.exists(st_path) else "widescreen=0\n"
    shutil.copy(AZ_CFG, AZ_CFG + ".pcprof.bak")
    samples = []
    try:
        open(AZ_CFG, "w").write(set_stub(cfg, True))
        open(st_path, "w").write("widescreen=0\n" + "".join(s + "\n" for s in settings))
        subprocess.run("pkill -9 -f MacOS/azahar", shell=True)
        time.sleep(1)
        app = subprocess.run("ls -d /Applications/*[Aa]zahar*/Azahar.app | head -1", shell=True,
                             capture_output=True, text=True).stdout.strip()
        subprocess.run(["open", "-n", "-a", app, "--args", ROM])
        g = GdbRsp(port=PORT, timeout=30.0, connect_wait=90.0)
        # '?' first: Azahar's stub crashes (the whole emulator) on an interrupt when no halt query selected a thread
        g.cmd("?")
        g.send("c")  # the stub holds the boot until a debugger continues it
        time.sleep(args.warmup)
        for _ in range(args.samples):
            # the stop reply carries the registers: T05 0f:<pc>;0d:<sp>;0e:<lr>;thread:N; (little-endian hex)
            r = g.interrupt()
            regs = {}
            for field in r[3:].split(";"):
                if ":" in field:
                    k, v = field.split(":", 1)
                    if len(k) == 2 and len(v) == 8:
                        regs[int(k, 16)] = int.from_bytes(bytes.fromhex(v), "little")
            if 15 in regs:
                samples.append((regs[15], regs.get(14, 0)))
            g.send("c")
            time.sleep(args.interval)
        g.detach()
    finally:
        open(AZ_CFG, "w").write(cfg)
        open(st_path, "w").write(st_old)
        subprocess.run("pkill -9 -f MacOS/azahar", shell=True)

    addrs, names = symbols()
    fn = collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    for pc, lr in samples:
        f = lookup(addrs, names, pc)
        fn[f] += 1
        callers[f][lookup(addrs, names, lr & ~1)] += 1
    # kernel calls: threads waiting, and the emulator's own work (GPU emulation inside GSP requests) - not CPU work
    busy = {f: c for f, c in fn.items() if not f.startswith("svc")}
    n = sum(busy.values()) or 1
    print("%d samples, %d in user code (svc* excluded: waits and emulator-side service work)" % (len(samples), n))
    for f, c in sorted(busy.items(), key=lambda kv: -kv[1])[:args.top]:
        top_callers = ", ".join("%s %d%%" % (k, 100 * v // c) for k, v in callers[f].most_common(3))
        print("%6.2f%%  %-40s  <- %s" % (100.0 * c / n, f[:40], top_callers))
    for want in [w for w in args.lines.split(",") if w]:
        pcs = collections.Counter(pc for pc, _ in samples if lookup(addrs, names, pc) == want)
        if not pcs:
            continue
        a2l = subprocess.run(["/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line", "-i", "-e", ELF] +
                             ["0x%x" % pc for pc in pcs], capture_output=True, text=True).stdout.split("\n")
        # with -i one address may print several lines (inlined frames): take the first (innermost)
        out, it = collections.Counter(), iter(a2l)
        lines_by_pc = {}
        res = subprocess.run(["/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line", "-e", ELF] +
                             ["0x%x" % pc for pc in pcs], capture_output=True, text=True).stdout.split("\n")
        for pc, line in zip(pcs, res):
            out[os.path.basename(line.split(" ")[0])] += pcs[pc]
        tot = sum(pcs.values())
        print("\n%s: %d samples" % (want, tot))
        for line, c in out.most_common(25):
            print("   %5.1f%%  %s" % (100.0 * c / tot, line))


if __name__ == "__main__":
    main()

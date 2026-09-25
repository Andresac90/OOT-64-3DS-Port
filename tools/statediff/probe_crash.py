#!/usr/bin/env python3
"""probe_crash.py - does the ORIGINAL game (N64 reference in ares) hang or crash in this scene?

Boots the reference ROM straight into an entrance (debug save, given age), follows Play_Update for
--seconds, and if the game thread stops, reports libultra's faulted thread: exception code, PC and the
calling function. Used to tell port bugs from original-game crashes the port need not reproduce
(e.g. child Volvagia entry: FPE in Message_DrawText).

usage: probe_crash.py --entrance 0x145 --age adult [--seconds 60]
"""
import argparse, os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import statediff as SD  # noqa: E402
import rngtrace as RT  # noqa: E402
from gdbrsp import GdbRsp  # noqa: E402

EXC = {0: "interrupt", 1: "TLB mod", 2: "TLB load/fetch", 3: "TLB store", 4: "address error load",
       5: "address error store", 10: "reserved instruction", 15: "floating point"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entrance", required=True)
    ap.add_argument("--age", default="child", choices=["adult", "child"])
    ap.add_argument("--seconds", type=int, default=60)
    a = ap.parse_args()
    age = SD.AGE_IDS[a.age]
    SD.write_tour_header()
    rom, elf = SD.ref_rom(a.entrance, "idle", tag="%s_idle_age%s" % (a.entrance, age), age=age)
    syms = SD.n64_symbols(elf)
    sym = RT.Symbolizer(SD.MIPS_NM, elf)
    lay = SD.load_layout()
    frames_off = SD.field_off(lay, "PlayState", "gameplayFrames")
    SD.sh("pkill -9 -f MacOS/ares")
    time.sleep(1)
    subprocess.Popen([SD.ARES, "--system", "Nintendo 64", "--no-file-prompt", rom],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    g = GdbRsp(port=9123, connect_wait=30)
    anchor = syms["StateDiff_Sync"]
    g.set_break(anchor)
    last, t0, play = None, time.time(), None
    try:
        g.cont_until(anchor, timeout=120)
        while time.time() - t0 < a.seconds:
            play = g.read_regs()[4] & 0xFFFFFFFF
            last = struct.unpack(">I", g.read_mem(play + frames_off, 4))[0]
            g.cont_until(anchor, timeout=20, from_addr=anchor)
        print("OK: game still running after %ds (gameplay frame %s)" % (a.seconds, last))
    except (TimeoutError, OSError):
        print("STOPPED: no Play_Update for 20s after gameplay frame %s" % last)
        g.interrupt()
        if play:
            rd = lambda p, n: int.from_bytes(g.read_mem(play + SD.field_off(lay, "PlayState", p), n), "big")
            print("message state: textId 0x%04x msgMode %d" % (rd("msgCtx.textId", 2), rd("msgCtx.msgMode", 1)))
        ft = struct.unpack(">I", g.read_mem(syms["__osFaultedThread"], 4))[0]
        if not ft:
            print("no faulted thread: game thread blocked (hang), not a CPU exception")
        else:
            ctx = g.read_mem(ft + 0x20, 0x110)  # OSThread.context: __OSThreadContext
            sr, pc, cause, badv = struct.unpack(">IIII", ctx[0xF8:0x108])
            ra = struct.unpack(">I", ctx[0xE4:0xE8])[0]
            exc = (cause >> 2) & 31
            print("FAULT: %s (exc %d) at %08x %s, badvaddr %08x, called from %08x %s" %
                  (EXC.get(exc, "?"), exc, pc, sym(pc), badv, ra, sym(ra)))
    finally:
        g.detach()
        SD.sh("pkill -9 -f MacOS/ares")


if __name__ == "__main__":
    main()

#!/bin/bash
# perfbench.sh — repeatable CPU-cost benchmark in Azahar (2026-10-01).
# Azahar's emulated clock advances per executed ARM instruction, so its timings are an
# instruction-count proxy (no cache stalls / VFP latency: hardware is ~1.5-2.8x slower).
# Lowering these numbers lowers hardware time on both consoles. Workload: the title-screen
# attract demo (same content every boot). Builds must be normal (not statediff/tour builds).
#   tools/perfbench.sh LABEL [secs] [extra settings lines...]
# Prints per-report averages of the perf/prof lines from boot.log and appends them to
# build/perfbench.log.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
LABEL="${1:-run}"; SECS="${2:-100}"; shift 2 2>/dev/null || shift $#
SD="$HOME/Library/Application Support/Azahar/sdmc/3ds/oot"
SETTINGS="$SD/settings.txt"
cp "$SETTINGS" /tmp/perfbench_settings.bak 2>/dev/null
{ echo "widescreen=0"; echo "prof=1"; for l in "$@"; do echo "$l"; done; } > "$SETTINGS"
timeout $((SECS + 120)) "$REPO/tools/emu.sh" boot "$SECS" > /tmp/perfbench_emu.out 2>&1
cp /tmp/perfbench_settings.bak "$SETTINGS" 2>/dev/null || echo "widescreen=0" > "$SETTINGS"
python3 - "$SD/boot.log" "$LABEL" "$REPO/build/perfbench.log" <<'EOF'
import re, sys
log, label, out = sys.argv[1:4]
reps, cur = [], {}
for l in open(log, errors="ignore"):
    m = re.match(r"(perf [^=]*|prof [^=]*)=0x([0-9a-f]+)", l.strip())
    if not m: continue
    k, v = m.group(1), int(m.group(2), 16)
    if k == "perf updates/s x10" and cur:
        reps.append(cur); cur = {}
    if k.startswith("perf dl top opcode") or k.startswith("perf dl op<<") or k.startswith("perf audio op<<"):
        continue
    cur[k] = v
if cur: reps.append(cur)
reps = reps[1:]  # first report = boot / logo
if not reps: print("no reports"); sys.exit(1)
keys = [k for k in reps[0] if all(k in r for r in reps)]
lines = ["== %s (%d reports)" % (label, len(reps))]
for k in keys:
    vals = [r[k] for r in reps]
    lines.append("%-48s %10.1f" % (k, sum(vals) / len(vals)))
txt = "\n".join(lines)
print(txt)
open(out, "a").write(txt + "\n")
EOF

#!/bin/bash
# regress.sh — one-shot regression check for the OoT 3DS port (macOS/Azahar).
# Boots the CURRENT build (build/3ds/oot.3ds) and prints a definitive verdict + exit code:
#   PASS = reached the render loop, no crash markers, AND frames still advancing after render.
#   FAIL = a crash/runaway marker, OR a hang before render, OR a stall/crash after render.
# Catches all three failure modes seen in dev:
#   (1) pre-render hang (e.g. the font-marker crash, the entrance-override hang),
#   (2) crash/runaway markers in boot.log,
#   (3) post-render crash/dialog (e.g. the audio-load crash) — detected as frame-ticks stalling.
# Run it after every change: `tools/regress.sh [label]`. Build first (make -f Makefile.3ds cci).
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ROM="${ROM:-$REPO/build/3ds/oot.3ds}"
AZ="${AZAHAR_BIN:-$(ls /Applications/*[Aa]zahar*/Azahar.app/Contents/MacOS/azahar 2>/dev/null | head -1)}"
SD="$HOME/Library/Application Support/Azahar/sdmc/3ds/oot"
BOOTLOG="$SD/boot.log"
LABEL="${1:-run}"
CRASH_RE='bogus|OOB|RUNAWAY|Fatal|stop unmapped|assert|abort'
RENDER_TIMEOUT="${RENDER_TIMEOUT:-55}"   # seconds to reach the render loop
LIVENESS_WAIT="${LIVENESS_WAIT:-8}"      # seconds to confirm frames keep advancing after render

fail() { echo "FAIL($LABEL): $*"; pkill -9 -f "MacOS/azahar" >/dev/null 2>&1; exit 1; }
[ -x "$AZ" ] || fail "azahar not found (set AZAHAR_BIN)"
[ -f "$ROM" ] || fail "ROM not built: $ROM  (run: make -f Makefile.3ds cci)"

pkill -9 -f "MacOS/azahar" >/dev/null 2>&1; sleep 1
: > "$BOOTLOG"
# Orphan Azahar (subshell backgrounds it then exits) so it keeps window-server access
# while this script polls; a script that stays the parent would lose GUI access on macOS.
( "$AZ" "$ROM" >/tmp/regress_az.out 2>&1 & )

# Phase 1: reach the render loop, or catch an early crash/hang.
seen=""
for i in $(seq 1 "$RENDER_TIMEOUT"); do
  sleep 1
  if grep -qaE "$CRASH_RE" "$BOOTLOG" 2>/dev/null; then
    fail "crash before/at render: $(grep -aoE "$CRASH_RE" "$BOOTLOG" | head -1) | last: $(grep -aE 'Play|graph:' "$BOOTLOG" | tail -1)"
  fi
  if grep -qa "Interface_Init" "$BOOTLOG" 2>/dev/null; then seen=1; break; fi
done
[ -n "$seen" ] || fail "hang — no render in ${RENDER_TIMEOUT}s | last: $(grep -aE 'Play|graph:' "$BOOTLOG" | tail -1)"

# Phase 2: confirm liveness (catches a crash/dialog that halts the main thread after render).
c1=$(grep -ac "frame tick" "$BOOTLOG"); sleep "$LIVENESS_WAIT"; c2=$(grep -ac "frame tick" "$BOOTLOG")
grep -qaE "$CRASH_RE" "$BOOTLOG" 2>/dev/null && fail "crash after render: $(grep -aoE "$CRASH_RE" "$BOOTLOG" | head -1)"
[ "$c2" -gt "$c1" ] || fail "stalled after render (crash/dialog?) — frame ticks ${c1}->${c2}"

echo "PASS($LABEL): rendered + alive — frame ticks ${c1}->${c2}, $(wc -l < "$BOOTLOG") log lines"
pkill -9 -f "MacOS/azahar" >/dev/null 2>&1
exit 0

#!/usr/bin/env bash
# emu-test.sh — macOS/Azahar autonomous test loop for the OoT 3DS port.
# Reconstruction of the old Windows tools/emu-test.ps1 (see PORT_ROADMAP.md §0.5).
#
# What it does, one cycle (~30-40s):
#   1. kills any running Azahar
#   2. clears the emulated SD boot.log  (sdmc:/3ds/oot/boot.log)
#   3. boots oot.3ds (CCI direct-boot; NEVER the .cia -> it prompts an install dialog)
#   4. optionally taps 3DS buttons (-p "START,A")
#   5. waits N seconds, screenshots the Azahar window, kills Azahar
#   6. prints a boot.log summary (lines / DL problems / tex-skips / tail)
#
# Usage:
#   tools/emu-test.sh -s 30 -n hyrule            # boot 30s, save shot tools/shots/hyrule.png
#   tools/emu-test.sh -s 20 -n pause -p "START"  # boot, tap START at ~t/2
#   ROM=build/3ds/oot.3ds tools/emu-test.sh -n boot
#
# Requires: Azahar.app installed. First run auto-detects paths; override with env vars below.
set -euo pipefail

# ---- config (override via env) ----------------------------------------------
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROM="${ROM:-$REPO_DIR/build/3ds/oot.3ds}"
# Azahar app: auto-detect a versioned folder or the standard install; override with AZAHAR_APP.
AZAHAR_APP="${AZAHAR_APP:-$(ls -d /Applications/*[Aa]zahar*/Azahar.app /Applications/Azahar.app 2>/dev/null | head -1)}"
AZAHAR_BIN="${AZAHAR_BIN:-$AZAHAR_APP/Contents/MacOS/azahar}"
# Azahar's portable data / SD root. Default install location on macOS:
AZAHAR_USER="${AZAHAR_USER:-$HOME/Library/Application Support/Azahar}"
SD_ROOT="${SD_ROOT:-$AZAHAR_USER/sdmc}"
BOOTLOG="${BOOTLOG:-$SD_ROOT/3ds/oot/boot.log}"
SHOT_DIR="${SHOT_DIR:-$REPO_DIR/tools/shots}"

# 3DS-button -> Azahar/Citra default keyboard key (tune to your input profile).
declare -A KEYMAP=(
  [A]=a [B]=s [X]=z [Y]=x [L]=q [R]=w
  [START]=m [SELECT]=n
  [UP]=t [DOWN]=g [LEFT]=f [RIGHT]=h            # D-pad
  [CUP]=i [CDOWN]=k [CLEFT]=j [CRIGHT]=l        # C-stick / camera (if mapped)
)

# ---- args -------------------------------------------------------------------
SECONDS_RUN=30; SHOT_NAME="shot"; PRESS=""
while getopts "s:n:p:h" opt; do
  case "$opt" in
    s) SECONDS_RUN="$OPTARG" ;;
    n) SHOT_NAME="$OPTARG" ;;
    p) PRESS="$OPTARG" ;;
    h) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "bad option"; exit 2 ;;
  esac
done

# ---- preflight --------------------------------------------------------------
[[ -x "$AZAHAR_BIN" ]] || { echo "!! Azahar not found at: $AZAHAR_BIN
   Install it, or set AZAHAR_APP=/path/to/Azahar.app"; exit 1; }
[[ -f "$ROM" ]] || { echo "!! ROM not found: $ROM  (build it, or set ROM=...)"; exit 1; }
mkdir -p "$SHOT_DIR" "$(dirname "$BOOTLOG")"

kill_azahar() { pkill -x azahar 2>/dev/null || true; sleep 1; }

send_key() { # $1 = single key char
  osascript -e "tell application \"System Events\" to keystroke \"$1\"" 2>/dev/null || true
}
tap_buttons() { # $1 = "START,A,..."
  IFS=',' read -ra btns <<< "$1"
  for b in "${btns[@]}"; do
    b="$(echo "$b" | tr '[:lower:]' '[:upper:]' | xargs)"
    key="${KEYMAP[$b]:-}"
    [[ -z "$key" ]] && { echo "   (no keymap for $b, skipped)"; continue; }
    echo "   tap $b ($key)"
    # hold ~250ms so the ~30fps input poll registers it
    osascript -e "tell application \"System Events\" to key down \"$key\"" 2>/dev/null || true
    sleep 0.25
    osascript -e "tell application \"System Events\" to key up \"$key\"" 2>/dev/null || true
    sleep 0.2
  done
}

screenshot() { # $1 = output png ; capture the frontmost Azahar window, fallback full-screen
  local out="$1" wid
  wid="$(osascript -e 'tell application "System Events" to tell (first process whose name is "azahar") to get id of front window' 2>/dev/null || true)"
  if [[ -n "$wid" && "$wid" =~ ^[0-9]+$ ]]; then
    screencapture -x -o -l "$wid" "$out" 2>/dev/null && return 0
  fi
  screencapture -x "$out" 2>/dev/null || true   # whole screen fallback
}

summarize_log() {
  if [[ ! -f "$BOOTLOG" ]]; then echo "   (no boot.log written — SD mapping or PortDbg path?)"; return; fi
  local n dl tex
  n="$(wc -l < "$BOOTLOG" | xargs)"
  dl="$(grep -ciE 'runaway|garbage DL|DL problem|\[GFX\]' "$BOOTLOG" || true)"
  tex="$(grep -ciE 'tex.?skip|unmapped tex|import_texture.*skip' "$BOOTLOG" || true)"
  echo "   boot.log: $n lines | DL-problems: $dl | tex-skips: $tex"
  echo "   --- tail ---"; tail -n 12 "$BOOTLOG" | sed 's/^/   /'
}

# ---- run --------------------------------------------------------------------
echo ">> emu-test: $(basename "$ROM")  ${SECONDS_RUN}s  shot=$SHOT_NAME  press='${PRESS:-none}'"
kill_azahar
: > "$BOOTLOG" 2>/dev/null || rm -f "$BOOTLOG" 2>/dev/null || true
echo ">> booting..."
"$AZAHAR_BIN" "$ROM" >/dev/null 2>&1 &
AZ_PID=$!
sleep 3   # let it spin up / start the game

if [[ -n "$PRESS" ]]; then sleep "$(( SECONDS_RUN / 2 ))"; tap_buttons "$PRESS"; sleep "$(( SECONDS_RUN / 2 ))"
else sleep "$SECONDS_RUN"; fi

OUT="$SHOT_DIR/${SHOT_NAME}.png"
echo ">> screenshot -> $OUT"
screenshot "$OUT"
kill_azahar
kill "$AZ_PID" 2>/dev/null || true

echo ">> summary:"; summarize_log
echo ">> done. shot: $OUT"

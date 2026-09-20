#!/bin/bash
# emu.sh — Azahar debug harness for the OoT 3DS port (macOS).
# Written for the stock macOS /bin/bash (3.2) — NO associative arrays, NO `declare -A`.
#
# Subcommands (ROM defaults to build/3ds/oot.3ds; override with ROM=...):
#   boot   [secs]          Boot, run `secs` (default 40), print a boot.log verdict.
#   record NAME [secs]     Boot recording a TAS movie to tools/movies/NAME.ctm.
#                          Play until it exits or `secs` (default 180) elapse, then the
#                          movie is saved. Use this to capture a bug repro.
#   play   NAME [secs]     Deterministically replay tools/movies/NAME.ctm, dumping video
#                          to tools/captures/NAME.webm, then print a boot.log verdict.
#   smoke  NAME [secs]     Boot + send a scripted button sequence (needs macOS
#                          Accessibility granted to Terminal) + screenshots + verdict.
#
# Verdict = last boot.log marker, crash/hang markers, whether Play_Init completed and
# frames are ticking. The emulated SD's boot.log is the source of truth.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ROM="${ROM:-$REPO/build/3ds/oot.3ds}"
AZAHAR_APP="${AZAHAR_APP:-$(ls -d /Applications/*[Aa]zahar*/Azahar.app 2>/dev/null | head -1)}"
AZAHAR_BIN="${AZAHAR_BIN:-$AZAHAR_APP/Contents/MacOS/azahar}"
SD_ROOT="${SD_ROOT:-$HOME/Library/Application Support/Azahar/sdmc}"
BOOTLOG="${BOOTLOG:-$SD_ROOT/3ds/oot/boot.log}"
MOVIE_DIR="$REPO/tools/movies"
CAP_DIR="$REPO/tools/captures"

die() { echo "!! $*" >&2; exit 1; }
[ -x "$AZAHAR_BIN" ] || die "Azahar not found at: $AZAHAR_BIN (set AZAHAR_APP=/path/to/Azahar.app)"
[ -f "$ROM" ] || die "ROM not found: $ROM (build it: make -f Makefile.3ds cci)"
mkdir -p "$MOVIE_DIR" "$CAP_DIR" "$(dirname "$BOOTLOG")"

kill_azahar() { pkill -9 -f azahar >/dev/null 2>&1; sleep 1; }
clear_log()   { : > "$BOOTLOG" 2>/dev/null || rm -f "$BOOTLOG" 2>/dev/null || true; }

# Bash-3.2-safe keymap (Azahar/Citra default keyboard profile — recalibrate to yours).
key_for() {
  case "$1" in
    A) echo a;; B) echo s;; X) echo z;; Y) echo x;;
    L) echo q;; R) echo w;; START) echo m;; SELECT) echo n;;
    UP) echo t;; DOWN) echo g;; LEFT) echo f;; RIGHT) echo h;;  # D-pad
    CPUP) echo up;; CPDOWN) echo down;; CPLEFT) echo left;; CPRIGHT) echo right;;  # circle pad
    *) echo "";;
  esac
}
tap() { # tap BUTTON [hold_seconds]
  local k; k="$(key_for "$1")"; [ -z "$k" ] && { echo "   (no key for $1)"; return; }
  local hold="${2:-0.3}"
  osascript -e "tell application \"System Events\" to key down \"$k\"" >/dev/null 2>&1 \
    || { echo "   !! keystroke blocked — grant Terminal Accessibility (Settings > Privacy & Security)"; return 1; }
  sleep "$hold"
  osascript -e "tell application \"System Events\" to key up \"$k\"" >/dev/null 2>&1
  sleep 0.15
}

verdict() {
  echo ">> verdict:"
  [ -f "$BOOTLOG" ] || { echo "   (no boot.log — SD mapping wrong?)"; return; }
  local n crash
  n="$(wc -l < "$BOOTLOG" | tr -d ' ')"
  crash="$(grep -acE 'bogus|OOB|RUNAWAY|Fatal|abort|stop unmapped|assert' "$BOOTLOG" 2>/dev/null)"
  echo "   boot.log lines : $n"
  echo "   crash markers  : $crash  (0 = clean)"
  if grep -qa 'Interface_Init' "$BOOTLOG" 2>/dev/null; then
    echo "   Play_Init      : COMPLETED (reached render loop)"
  elif grep -qa 'Play_Init: enter' "$BOOTLOG" 2>/dev/null; then
    echo "   Play_Init      : STARTED but did NOT complete — likely hang here"
  else
    echo "   Play_Init      : not reached"
  fi
  echo "   last marker    : $(grep -aE 'Play|graph:|Interface|SpawnScene' "$BOOTLOG" 2>/dev/null | tail -1)"
  [ "$crash" -gt 0 ] && { echo "   --- crash context ---"; grep -anE 'bogus|OOB|RUNAWAY|Fatal|abort|stop unmapped|assert' "$BOOTLOG" | tail -5 | sed 's/^/   /'; }
}

SECS="${3:-}"
case "${1:-}" in
  boot)
    SECS="${2:-40}"; kill_azahar; clear_log
    echo ">> boot $(basename "$ROM") for ${SECS}s"
    nohup "$AZAHAR_BIN" "$ROM" >/tmp/azahar.out 2>&1 & disown
    sleep "$SECS"; verdict; kill_azahar ;;
  record)
    # Azahar's parser is short-flag getopt (--movie-record gets mangled) -> use -r.
    # IMPORTANT: the .ctm is only written on a CLEAN quit (Cmd+Q / close the window),
    # NOT on SIGTERM/SIGKILL. So this launches Azahar in the FOREGROUND and waits for
    # YOU to close it; that's when the movie saves. (Do the bug repro, then quit Azahar.)
    NAME="${2:?usage: emu.sh record NAME}"; kill_azahar; clear_log
    echo ">> RECORDING to $MOVIE_DIR/$NAME.ctm — play the repro, then QUIT Azahar (Cmd+Q) to save."
    "$AZAHAR_BIN" -r "$MOVIE_DIR/$NAME.ctm" "$ROM"
    echo ">> saved: $MOVIE_DIR/$NAME.ctm ($(ls -la "$MOVIE_DIR/$NAME.ctm" 2>/dev/null | awk '{print $5}') bytes)" ;;
  play)
    NAME="${2:?usage: emu.sh play NAME [secs]}"; SECS="${3:-60}"
    [ -f "$MOVIE_DIR/$NAME.ctm" ] || die "no movie: $MOVIE_DIR/$NAME.ctm (record it first)"
    kill_azahar; clear_log
    echo ">> REPLAY $NAME.ctm for ${SECS}s"
    nohup "$AZAHAR_BIN" -p "$MOVIE_DIR/$NAME.ctm" "$ROM" >/tmp/azahar.out 2>&1 & disown
    sleep "$SECS"; verdict; kill_azahar ;;
  smoke)
    NAME="${2:-smoke}"; SECS="${3:-45}"; kill_azahar; clear_log
    echo ">> smoke '$NAME' — boot + scripted inputs (needs Terminal Accessibility)"
    nohup "$AZAHAR_BIN" "$ROM" >/tmp/azahar.out 2>&1 & disown
    sleep 20   # let it reach Hyrule Field
    open -a "$AZAHAR_APP" >/dev/null 2>&1; sleep 1
    # scripted sequence: pause menu tour, then walk each way + interact
    tap START 0.3 || { verdict; kill_azahar; exit 1; }
    tap CPRIGHT 0.5; tap CPLEFT 0.5; tap A 0.3; tap START 0.3
    tap CPUP 0.8; tap CPDOWN 0.8; tap CPLEFT 0.8; tap CPRIGHT 0.8
    tap A 0.3; tap B 0.3
    sleep 3
    screencapture -x "$CAP_DIR/${NAME}.png" 2>/dev/null; echo "   shot: $CAP_DIR/${NAME}.png"
    verdict; kill_azahar ;;
  *)
    grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac

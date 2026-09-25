#!/bin/bash
# make_ref.sh [ENTRANCE] - build the N64 reference ROM for tools/statediff.
# Reference = pristine upstream decomp (UPSTREAM commit, proven to match the retail US ROM md5)
# plus ONLY (1) the port's boot bypass in z_opening.c (debug save -> Play, optional start entrance
# with child Link), (2) zeroing each gamestate at allocation (graph.c) and (3) a fixed RNG seed
# (z_play.c), so both sides start from the same state. Output: build/statediff/ref_<tag>.{z64,elf}
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
UPSTREAM=269d03016
WT="$REPO/../OOT-64-3DS-Port-n64ref"
ENTR="${1:-}"
SCRIPT_H="${2:-}"          # generated input_script.h (scripted controller input); tour_script.h beside it
TAG="${3:-${ENTR:-default}}"
AGE="${4:-}"               # optional LINK_AGE_* for the boot (scene tour)
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v miniconda | paste -sd: -)   # conda's libxml2 breaks tools/audio

if [ ! -d "$WT" ]; then
  git -C "$REPO" worktree add --detach "$WT" "$UPSTREAM"
  cp "$REPO"/baseroms/ntsc-1.0/baserom*.z64 "$WT/baseroms/ntsc-1.0/"
  ln -s "$REPO/.venv" "$WT/.venv"
  gmake -C "$WT/tools" -j8 >/dev/null
  gmake -C "$WT" setup VERSION=ntsc-1.0 -j8 >/dev/null
fi
F=src/overlays/gamestates/ovl_opening/z_opening.c
G=src/code/graph.c
P=src/code/z_play.c
git -C "$WT" checkout -q "$UPSTREAM" -- "$F" "$G" "$P"
# same as the port's PORT_STATEDUMP builds: fixed RNG seed instead of osGetTime()
sed -i '' 's/    Rand_Seed((u32)osGetTime());/    Rand_Seed(0x5EED0000);/' "$WT/$P"
grep -q "Rand_Seed(0x5EED0000)" "$WT/$P" || { echo "upstream z_play.c changed"; exit 1; }
# same scripted input as the port: identical statediff_input.h, called at the start of Play_Update
if [ -n "$SCRIPT_H" ]; then
  cp "$REPO/tools/statediff/statediff_input.h" "$WT/src/code/statediff_input.h"
  cp "$SCRIPT_H" "$WT/src/code/input_script.h"
  cp "$(dirname "$SCRIPT_H")/tour_script.h" "$WT/src/code/tour_script.h"
  python3 - "$WT/$P" <<'PY3'
import sys
p = sys.argv[1]
s = open(p).read()
head = "void Play_Update(PlayState* this) {\n    Input* input = this->state.input;\n    s32 isPaused;\n    s32 pad1;\n"
assert s.count(head) == 1, "upstream Play_Update changed"
s = s.replace(head, '#include "statediff_input.h"\n\n' + head + "\n    StateDiff_InjectInput(this);\n")
open(p, "w").write(s)
PY3
fi
# same as the port's PORT_STATEDUMP builds: zero each gamestate at allocation (no leftover bytes)
python3 - "$WT/$G" <<'PY2'
import sys
p = sys.argv[1]
s = open(p).read()
old = '        gameState = SYSTEM_ARENA_MALLOC(size, "../graph.c", 1196);\n'
assert s.count(old) == 1, "upstream graph.c changed"
open(p, "w").write(s.replace(old, old + "        if (gameState != NULL) {\n            bzero(gameState, size);\n        }\n"))
PY2
python3 - "$WT/$F" "$ENTR" "$AGE" <<'PY'
import sys
p, entr, age = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
old = """    gSaveContext.gameMode = GAMEMODE_TITLE_SCREEN;
    this->state.running = false;
    gSaveContext.save.linkAge = LINK_AGE_ADULT;
    Sram_InitDebugSave();
    gSaveContext.save.cutsceneIndex = CS_INDEX_3;
    // assigning scene layer here is redundant, as Play_Init sets it right away
    gSaveContext.sceneLayer = GET_CUTSCENE_LAYER(CS_INDEX_3);
"""
new = """    // statediff reference: same boot bypass as the 3DS port (z_opening.c __3DS__ block)
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    this->state.running = false;
    gSaveContext.save.linkAge = LINK_AGE_ADULT;
    Sram_InitDebugSave();
    gSaveContext.save.cutsceneIndex = 0;
    gSaveContext.sceneLayer = 0;
    gSaveContext.fileNum = 0;
"""
if entr:
    new += "    gSaveContext.save.linkAge = LINK_AGE_CHILD;\n    gSaveContext.save.entranceIndex = %s;\n" % entr
if age:
    new += "    gSaveContext.save.linkAge = %s;\n" % age
assert s.count(old) == 1, "upstream z_opening.c changed"
open(p, "w").write(s.replace(old, new))
PY
gmake -C "$WT" rom VERSION=ntsc-1.0 REGION=US -j8 >"$REPO/build/statediff/ref_build.log" 2>&1 || { tail -20 "$REPO/build/statediff/ref_build.log"; exit 1; }
git -C "$WT" checkout -q "$UPSTREAM" -- "$F" "$G" "$P"
rm -f "$WT/src/code/statediff_input.h" "$WT/src/code/input_script.h" "$WT/src/code/tour_script.h"
mkdir -p "$REPO/build/statediff"
cp "$WT/build/ntsc-1.0/oot-ntsc-1.0.z64" "$REPO/build/statediff/ref_$TAG.z64"
cp "$WT/build/ntsc-1.0/oot-ntsc-1.0.elf" "$REPO/build/statediff/ref_$TAG.elf"
echo "reference ready: build/statediff/ref_$TAG.z64 (+ .elf)"

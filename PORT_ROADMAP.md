# OoT → New 3DS Port — Status, Architecture & Roadmap (v2)

> **Purpose:** the canonical handoff document for anyone (human or AI) continuing this port.
> Read this FIRST. Every claim below was **verified against the actual code on 2026-07-22**
> by a 4-agent full-codebase audit (renderer / game patches + 2D UI / audio / stability +
> memory + perf). File:line references are current as of that date.
> Last updated: 2026-08-01. Current build: audio microcode wired (gated off, gameplay unchanged).
>
> **§8.3 AUDIO — the one remaining subsystem (2026-08-01).** Research complete: the port
> is ~95% done; the whole audio ENGINE (src/audio/) already runs. The only missing piece was
> the RSP audio-microcode executor: synthesis.c builds an Acmd list -> M_AUDTASK, but sched_shim
> ran nothing. WROTE `port/src/audio_microcode.c` — a C reimpl of aspMain (CLEARBUFF, DMEMMOVE,
> LOAD/SAVEBUFF, SETBUFF, MIXER, INTERLEAVE, LOADADPCM, ADPCM, RESAMPLE, ENVMIXER, SETLOOP).
> Hooked into sched_shim.c (M_AUDTASK) -> AI buffer -> Port3ds_AudioSubmitFrame (BE->LE) ->
> existing ndsp sink. Compiles/links/boots/runs the real Acmd list every frame with no crash/
> hang/regression. GATED OFF via `sPortAudioUcodeEnable` (default 0) so a wrong first-cut can't
> ship as noise. NEXT: validate PCM (ADPCM/resample/envmixer are first-cut) — add PCM stats to
> boot.log or diff on PC vs a reference emulator, since the Azahar screenshot loop can't capture
> audio; then flip the gate. On-HW playback needs sdmc:/3ds/dspfirm.cdc.
> = cutscene fixes + **D7 heap-vs-segment-8 collision FIX** (seg_addr + PortSegmentedToVirtual:
> a `0x08xxxxxx`-`0x0Fxxxxxx` value with low-24-bits ≥ 0x100000 is a native heap pointer, not a
> seg-8..F ref) — this removes the file-select DL-runaway crash class (43→0 guards) and is
> **verified NOT to regress gameplay** (Hyrule Field renders clean after a full recompile).
> The file-select still renders BLACK content (separate textures/combiner/draw issue, §5.2d) so
> the boot bypass stays. Superseded 33801F44 / F62DF917.
> (prior) 33801F44 (40.1 MB, 2026-07-23)
> = 9D74958A (kaleido + texleak + vrom base + sword + cutscene DESYNC fix) PLUS the
> **cutscene PACKED-FIELD byte-order fix** (§0.7): cutscene cameras + actor cues now decode
> correctly — the title demo flies over Hyrule Field properly (was black garbage), and all
> in-game cutscene cameras are fixed. Boot bypass kept (title demo plays but the file-select
> 2D UI still needs D7-secondary). Emulator-clean baseline.
> LESSON: a TEV/alpha change must be visually confirmed on the SPECIFIC affected surface
> before shipping — the emulator's dim spawn hid the sword case. Alpha combiner remains an
> open gap (G1) but needs per-surface validation, not a blanket ladder.
>
> **D7 REFRAMED (2026-07-22, emulator): the title-demo + all transitions now render with
> 0 garbage DLs / 0 tex-skips / 0 crashes — the vrom base-symbol fix RESOLVED the D7
> *rendering* defect.** What remains is the title-demo CUTSCENE SCRIPT DESYNC (Link stuck
> running in place, never progresses to logo/file-select) — a `CsCmd` script-advancement
> bug, separate/deep (prior sessions only guarded it). Camera structs (CsCmdCam=8B,
> CutsceneCameraPoint=16B) match N64 sizes, so the desync is elsewhere in the command
> stream. This is now the real blocker for the title screen AND in-game cutscenes; boot
> bypass stays until it's fixed. Next dedicated target.

---

## 0. HOW MUCH IS LEFT (the honest answer)

The high-risk engineering — *can the OoT decomp run, render, and be played natively on a
New 3DS at all* — is **done and de-risked**. The game boots to Hyrule Field, renders in
color, Link is fully modeled and animated, scene transitions work, input and saves work,
no known crashes. What remains is bounded work with known shapes:

| Track | What | Effort (focused sessions) |
|---|---|---|
| M2 — Renderer correctness | alpha combiner, missing muxes, >4KB textures, BRANCH_Z, blend modes | ~3-6 sessions |
| M2b — 2D UI / title / file-select (D7) | **root cause found**: 2 small fixes (see §5) | ~1-2 sessions |
| M2c — Stability | kaleido song-playback hard-lock defuse + **texture linearAlloc leak** | ~1-2 sessions |
| M3 — Audio (the long pole) | engine already compiles; net-new = C mixer (~17 ops, 11 portable from sm64) + Audiobank byte-swap | ~10-20 sessions; M3d mixer is the single biggest item |
| M4 — Ship polish | strip diagnostics, deadzone, perf flags, icon/banner | ~1-2 sessions |
| M5 — Full-game hardening | play through story; each area may surface 1-2 known-class bugs | ongoing, weeks of casual testing |
| Phase 5 — Stereoscopic 3D (user request) | **no infra exists yet** (single target, GFX_LEFT only) | ~2-4 sessions, after M4 |

**Bottom line: "playable, correct-looking, silent" ≈ 5-10 more sessions. "Finished port
with music and sound" ≈ that plus the audio track, realistically 4-8 weeks of continued
part-time sessions. Nothing left is research-risk; it is all execution.**

---

## 0.5 AUTONOMOUS EMULATOR TEST LOOP (set up 2026-07-22 — the force multiplier)

Most iteration no longer needs the user's real hardware (which stays the FINAL validator).
**Azahar** (Citra successor) is installed at `tools/azahar/…`, New-3DS mode, portable
`user/` dir; the emulated SD maps the game's `sdmc:/3ds/oot/boot.log` to a file we read live.

- **Build** emits `oot.3ds` (CCI) in addition to the CIA (`build-cia.sh`). **Boot the
  `.3ds`, never the `.cia`** — a CIA prompts an install dialog; the CCI direct-boots with
  zero clicks. (The `.3dsx` fails to load in Azahar.)
- **Test:** `powershell -File tools\emu-test.ps1 -Seconds 30 -Shot NAME [-Press "START,A"]`
  kills Azahar, clears boot.log, boots `oot-test.3ds`, waits, prints the boot.log summary
  (line count + DL-problem count + tex-skip count + tail), and saves a pixel-exact
  framebuffer screenshot to `tools/NAME.png` (Azahar's built-in Ctrl+P capture —
  occlusion-proof, only ever captures the emulator, never other windows).
- **Input:** `-Press` taps 3DS buttons (held ~250ms so the ~30fps input poll registers
  them; SendKeys taps are too fast). Names A B X Y START SELECT L R UP DOWN LEFT RIGHT.
- **Cycle time ≈ 3-4 min, fully autonomous.** Caveat: Azahar is HLE + desktop GPU, so
  final COLOR/perf/audio correctness still needs the real N3DS; but crashes, hangs,
  runaways, tex-skips, 2D-UI, combiner logic, and boot flow reproduce faithfully.
- Build 89BAC301 validated on emulator: **0 runaways, 0 tex-skips**, boots to gameplay,
  Start opens the pause path with no lock (kaleido fix holds).

---

## 0.7 CUTSCENE DESYNC — ROOT-CAUSED & FIXED (2026-07-23)

**The title demo froze and (the same bug) would hang every in-game story cutscene.**
Root cause: cutscene camera-point data is authored as **packed big-endian words** via
`CMD_BBH(continueFlag, roll, frame)` = `(continueFlag<<24)|(roll<<16)|frame`. On N64 (BE)
`continueFlag` lands at struct byte 0; on this **little-endian port** that packed value
stores with `continueFlag` at **byte 3**, so `CutsceneCameraPoint->continueFlag` read 0x00
and the `CS_CAM_STOP` (−1) spline terminator was never found → the camera spline read past
its end → the whole `Cutscene_ProcessScript` command stream desynced → `state=STOP` →
`cutsceneIndex` reset → cutscene stopped after one frame (Link frozen).
- **Diagnosis path (all via the emulator loop):** `Cutscene_UpdateScripted` runs every frame
  (z_play.c:1026); traced `cutsceneIndex` fff3→0 after 1 frame; per-command trace showed
  valid cmds 0–10 then garbage at 11; camera spline at i=10 read 48 points (no terminator);
  raw-dumped the script and decoded the camera points — terminator word `0xff000000` has the
  `0xFF` at byte 3, not byte 0. **Note: game files log to boot.log via `PortDbgX`, NOT
  `fprintf(stderr)` (that channel is dropped) — this cost several cycles.**
- **Fix (z_demo.c):** `CS_CAM_CONTINUE_FLAG(script)` macro reads the high byte
  (`(s8)(((const u32*)script)[0] >> 24)`) — correct on BE and LE — applied to both
  `CutsceneCmd_UpdateCamEyeSpline` and `CutsceneCmd_UpdateCamAtSpline`. Verified: cutscene
  now runs continuously (`state=RUN`, `idx=fff3` every frame), 0 desync guards.
- **✓ FIXED (2026-07-23, build 33801F44): the packed POSITION/field byte-order.**
  A one-time in-place normalization (`Cutscene_NormalizePackedFields`, z_demo.c) walks the
  script like `Cutscene_ProcessScript` and applies the correct **per-macro** transform to
  each packed word — NOT a uniform byte reverse (that restores BE order but the LE struct
  reads u16s LE → byte-swapped garbage → black camera). Transforms: `CMD_HH`=halfword swap
  `(v>>16)|(v<<16)`; `CMD_BBH`=`(v>>24)|((v>>16&0xff)<<8)|((v&0xffff)<<16)`;
  `CMD_HBB`=`(v>>16)|((v>>8&0xff)<<16)|((v&0xff)<<24)`. Idempotency: scene cutscenes use a
  single-slot guard (`gCsSceneNormalized`, reset in `Cutscene_InitContext` per scene load so
  reloads re-normalize but alt-header double-calls don't double-swap); static arrays use a
  persistent visited-set. `CS_CAM_CONTINUE_FLAG` reverted to native. VERIFIED on emulator:
  title demo is a moving Hyrule Field flythrough, 0 desync guards. In-game cutscene cameras
  + actor cues now correct. Boot bypass stays only because the file-select **2D UI**
  (D7-secondary) is still unmapped — the cutscene system itself is done.

---

## 1. What this project is

The Legend of Zelda: Ocarina of Time (zeldaret/oot decompilation, **NTSC 1.0 US**)
compiled **natively for ARM11**, running as New Nintendo 3DS homebrew (CIA). No emulation.
No Nintendo assets in the repo — the user's legally dumped, decompressed ROM is streamed
at runtime from `sdmc:/3ds/oot/baserom-decompressed.z64`.

- Build env: **WSL Ubuntu** (user `aceve`), repo `/home/aceve/oot-port`
  (Windows UNC: `\\wsl.localhost\Ubuntu\home\aceve\oot-port\`). Reference decomp
  (build artifacts + extracted assets): `/home/aceve/oot`.
- Toolchain: devkitARM + libctru + citro3d → makerom → `oot.cia`, copied to
  `C:\Users\aceve\Documents\ZELDA_64_3DS-PORT\oot.cia` after every build.
- Test loop: user installs CIA (FBI) on a homebrewed New 3DS, sends photos +
  `sdmc:/3ds/oot/boot.log` + Luma crash dump (`crash_dump_00000000.dmp`, auto-wiped
  each boot by `WipeCrashDumps()`).

### Architecture (decided, working — do not relitigate)
- **Game code:** decomp C compiled for ARM11 (`-std=gnu90`, NTSC 1.0, `F3DEX_GBI_2`,
  `-D__3DS__` gates all port patches).
- **Renderer:** fast3d interpreter `port/src/gfx/gfx_pc.c` (walks real F3DEX2 DLs) →
  PICA200 backend `port/src/gfx3ds/gfx_citro3d.c` + `gfx_3ds.c` (citro3d TEV).
  Upstream reference: github.com/mkst/sm64-port.
- **Threads:** never scheduled. Synchronous frame loop (`port/src/sched_shim.c`,
  `PortGfx_RunTask` in `3ds_main.c`). `osRecvMesg`/`osSendMesg` NEVER block
  (return −1 on empty) — `port/src/ultra_shims.c:28-63`.
- **DMA:** `port/src/dma_shim.c` serves DMA synchronously. VROM ranges in `gVromMap`
  (`port/src_gen/vrom_map.c`, 1004 entries, index 0 = manual gameplay_keep blob) copy
  from native little-endian compiled assets; everything else falls back to
  byte-swapped reads of the BE ROM streamed from SD.
- **Boot path (TEMPORARY):** `TitleSetup_SetupTitleScreen` patched to boot directly
  into a debug save in Hyrule Field because title demo + file-select render garbage
  (D7 — root cause now known, see §5). Revert per §5.3 once fixed.

### Build scripts
- WSL `~/bin/`: `build-3ds.sh` (full), `relink-3ds.sh` (fast), `build-cia.sh` (**ship command**).
- Windows `port-files/`: `rebuild-gamefile.sh <src/....c>` (one game TU → CIA),
  `rebuild-kaleido.sh`, `rebuild-input-save.sh`, `build-gpk-blob.sh`.
- Always report the CIA **MD5** to the user.
- GOTCHAS: never pass multi-line shell through `wsl.exe -- bash -c` (write .sh →
  `sed 's/\r$//' > /tmp/x.sh && bash /tmp/x.sh`). **NEVER `git checkout -- <decomp file>`**
  (HEAD is pristine upstream; port changes are working-tree edits — checkout destroys them).
  Files built outside `recompile-game.sh` (kaleido!) silently keep stale objects when
  global headers change — rebuild them explicitly.

---

## 2. Current state (hardware-verified)

- Boots into Hyrule Field, **fully playable**: walking, camera, collision, Z-targeting,
  rolling, items; scene transitions (Field ↔ Castle Town ↔ Kakariko ↔ interiors).
- All actors render with correct geometry + animation; Link fully modeled.
- HUD renders; input fully mapped; SRAM saves persist to `sdmc:/3ds/oot/save.bin`.
- **No known crashes.** Remaining failure classes: (a) visual correctness (combiner
  gaps, §4), (b) 2D UI screens (D7, §5), (c) un-defused audio-wait soft-locks (§6),
  (d) audio absent (§7), (e) a texture-memory leak that will bite long sessions (§6.2).

---

## 3. Root-caused bug classes (institutional knowledge — READ THIS)

Each cost a hardware test cycle. Do not reintroduce.

1. **KSEG0 address math.** N64 pointers (≥0x80000000) are STRIPPED (`& 0x1FFFFFFF`),
   never add 0x80000000. Central: `PortSegmentedToVirtual`
   (port/include/segmented_address.h); renderer mirror: `seg_addr` (gfx_pc.c).
   Pointers into the binary image [0x00100000, `__end__`) pass through untouched.
   ⚠ This passthrough is also implicated in D7 — see §5.2.
2. **Overlay relocation must be identity.** Statically linked overlays must NOT apply
   `ptr + loadedRamAddr - vramStart`. Sites fixed: z_actor.c, kaleido (stale-object),
   z_effect_soft_sprite.c:230. Grep-verified no other active sites.
3. **Mtx packing.** `guMtxF2L` packs element k at halfword k^1 on LE; fast3d reads
   exactly that. `Matrix_MtxFToMtx`/`Matrix_MtxToMtxF` (sys_matrix.c:546,700) delegate
   to guMtxF2L/guMtxL2F on `__3DS__`. Any new Mtx conversion MUST use guMtx layout.
4. **Native-pointer-vs-segment-offset in special DMAs** (was: exploded Link).
   Asset relocation rewrites `LinkAnimationHeader->segment` to a NATIVE pointer; N64
   math `RomStart + (seg & 0xFFFFFF)` reads garbage. Fixed in
   `AnimTaskQueue_AddLoadPlayerFrame` (z_skelanime.c:888). Audit any new special DMA.
5. **Audio NULL channels.** No audio thread → `seqPlayers[].channels[]` stay NULL; game
   derefs directly. Defense: `Audio_PortEnsureNullChannels()` (general.c:3160), called
   per frame from 3ds_main.c:101. Remove at M3e.
6. **Fixed-size renderer pools overflow silently.** color_combiner_pool now 512+u16+guard
   (gfx_pc.c:113,320-326); sShaderProgramPool 1024+u16+guard (gfx_citro3d.c:65,342-347).
   `sTexturePool[4096]` still only printfs and **returns id 0** on overflow
   (gfx_citro3d.c:399-406) — silent aliasing onto slot 0; watch it.
7. **TEV state leaks between draws.** TEV CONSTANT reset in updateShader
   (gfx_citro3d.c:193, stage-1 at :266). Keep.
8. **Diagnostics discipline.** Never ship per-frame logging (one 40-minute frame taught
   this). Probes self-limiting + removed once answered. Full strip list in §9.
9. **Audio seq-load `udf #0` "Undefined Instruction" crash — mechanism proven, real fix = Audioseq load.**
   ROOT-CAUSED 2026-09-21 (evidence-driven, not guessed). Symptom: hard crash, R0=0xDE,
   PC in `AudioLoad_AsyncLoadInner+0x2c` (`0019bc44: e7f000f0 udf #0`). Mechanism: the crash
   PC is the compiler's exhaustive-switch trap for `tableType ∉ {SEQUENCE,FONT,SAMPLE}=0/1/2`.
   `tableType` arrives = 0xDE via AudioSeq interpreter opcode `ASEQ_OP_SEQ_LDRES`
   (seqplayer.c:2130) which reads it as a **raw byte of sequence data** → ScriptLoad → AsyncLoad
   → AsyncLoadInner. Script-read fns (seqplayer.c:569-581) are byte-safe BE, so NOT an
   endianness desync. **Upstream (REFINED, second pass — NOT an init/zeroing bug):** `gAudioCtx`
   IS fully zeroed at `Audio_Init`→`AudioLoad_Init` (load.c:1436-1438), `seqPlayers[4]` is an
   inline array (audio.h:988), so every `enabled` starts false; `enabled=true` comes ONLY from
   SISPI (load.c:656) after a non-NULL seqData check. In NORMAL operation the interpreter is
   fully IDLE — probes proved `AudioSeq_ProcessSequences` (seqplayer.c:2150) is never called and
   SISPI never runs (synthesis gated off, §8.3). The crash needs a sequence to ACTUALLY START:
   SISPI → `AudioSeq_SkipForwardSequence` (load.c:662) runs the interpreter immediately on the
   just-loaded `seqData`, whose CONTENT is garbage (bad Audioseq DMA/relocation/load) → pc walks
   it → bogus LDRES 0xDE. **Layout-sensitive, razor's edge:** any code/link shift moves off the
   fatal layout into a benign one, so it's NOT reliably reproducible; looked like a phantom /
   "stack overflow" — DISPROVEN: bumping the CCI main stack (RSF `StackSize` 0x40000→0x100000,
   the real knob; `__stacksize__` is 3dsx-only and ignored by makerom) did NOT stop it.
   DEFENSE SHIPPED (committed): fail-safe guard at top of `AudioLoad_AsyncLoadInner` (load.c,
   `#ifdef __3DS__`) rejects invalid tableType → returns NULL instead of trapping (provably
   prevents the udf by construction). With it the port is STABLE regardless of layout (guard
   neutralizes the trap; normal op never runs the interpreter). **Real fix (M3): fix the
   Audioseq sequence-load path so a started sequence's seqData is valid bytecode** — force a real
   sequence start (don't chase the layout) and verify seqData content; relates to item 5 (NULL
   channels) and the audio arena (§8.3). Verify with `tools/regress.sh`.

---

## 4. RENDERER — verified state + complete gap table (M2)

**Verified working end-to-end (2026-07-22 audit):** two-cycle RGB fold
(G_SETCOMBINE cycle-1 decode gfx_pc.c:1712-1730 → cc_id bits 27-29 :937-947 →
shader_id slot bits 30-31 :278-301 → vertex packing gfx_citro3d.c:329-334 → TEV
stage 1 MODULATE(PREVIOUS, src) :262-275; save/restore at texrect/fillrect
:1399/1448/1470-1475). renderTwoColorTris constant keyed on color0Constant, constant
set on TEV stages 0 AND 1 (:637-684). Crash-proofing complete: DL-walk guard
(:1568-1581, 2M-step runaway), import_texture unmapped guard (:538-551), TEXB `^7`
endian fix on ALL 9 texture readers (:373-524).

### Gap table (all file:line current)

| # | Gap | Where | In-game symptom | Effort | Risk |
|---|-----|-------|-----------------|--------|------|
| G1 | **Alpha combiner mux ignored** — alpha hardwired to TEXEL0.a (textured) / shade.a; decoded alpha mux `c[1][*]` unused | gfx_citro3d.c:245-254 (mux dead at :144-148) | PRIM.a/ENV.a fades on textured actors don't fade (enemy dissolve, Poe/ghost transparency, fade-ins) → pop instead of fade | M | Med |
| G2 | **TEXEL1/second texture unusable** — one UV in vertex format; shader aliases outtc1=intex; import_texture(1) reuses tile-0 fmt/siz; only G_TX_RENDERTILE tracked | shader.v.pica:19-20; gfx_pc.c:991-1007,1200-1215; gfx_citro3d.c:127-128 | Skybox day/night lerp broken; field detail-blend broken | L | Med |
| G3 | **Lossy muxes → CC_0**: G_CCMUX_1, ENV_ALPHA, PRIM_ALPHA, SHADE_ALPHA, TEXEL1_ALPHA, PRIM_LOD_FRAC, COMBINED(c0), K4/K5, NOISE | gfx_pc.c:1264-1283 | Inverse blends, alpha-weighted blends, LOD detail fades collapse (water, magic, lens) | M | Med |
| G4 | Alpha 2-cycle not folded (RGB-only fold); tint forms ≠ `(COMBINED-0)*X+0` revert to cycle-0 | gfx_pc.c:1716-1729 | Subtle transparency errors on 2-cycle alpha chains | M | Low |
| G5 | **Dropped GBI opcodes** (silent, no default case): G_BRANCH_Z/G_RDPHALF_1, G_CULLDL, G_LOADUCODE, G_MODIFYVTX, G_LINE3D, S2DEX | switch gfx_pc.c:1585-1790 | LOD select never branches (possible double/omitted actor draws); no bbox cull (perf); S2DEX 2D content invisible | M | Med |
| G6 | CI palette bank ignored (set_tile drops `palette`; load_tlut ignores tile/high_index; CI4 always bank 0) | gfx_pc.c:1200-1232, 449-462 | Palette-swapped textures wrong colors | S | Low |
| G7 | **Textures >4096 B clamped** | gfx_pc.c:1256-1258 | Large textures load only first 4 KB → bottom missing/garbage (64×64 RGBA16 terrain, 32-bit faces) | M | Med |
| G8 | Blend modes binary (SRC_ALPHA/1-SRC_ALPHA vs opaque only) | gfx_citro3d.c:542-554; heuristics gfx_pc.c:958-968 | Additive glows (fire, magic, flares) render dark/normal-blended | M | Med |
| G9 | **No stereoscopic 3D infra at all** (single target, GFX_LEFT only, no IOD uniform) | gfx_3ds.c:89-90; shader.v.pica | 3D slider inert. Phase 5 = real work, not a toggle | L | Low |
| G10 | **No framebuffer capture / render-to-texture** (set_color_image only records addr) | gfx_pc.c:1478-1484; gfx_3ds.c:89-90 | Pause-menu prerender blur absent; Lens-of-Truth/motion-blur/Deku-flash afterimage broken. Simple full-screen fades DO work | L | High |
| G11 | Frame pacing: vsync 60, game logic 20Hz via R_UPDATE_RATE — OK; but `get_time()` returns 0.0 | gfx_3ds.c:126-148 | Pacing works today; timing hooks inert for future use | S | Low |
| G12 | Only 2 color inputs carried; num_inputs≥3 truncates (printf "more than 2!") | gfx_citro3d.c:174-175; gfx_pc.c:291 | Minority of surfaces lose one color term | M | Med |
| G13 | CC_LOD approximated from `v1->w` per-tri | gfx_pc.c:1058-1066 | Detail-blend seams | S | Low |
| G14 | Texture cache: 512 slots, invalidate-ALL on overflow (no LRU) | gfx_pc.c:344-352 | >512 unique textures/frame → re-upload thrash + **fuels the leak in §6.2** | M | Low |
| G15 | Dead legacy code + getenv probes on hot path (do_single/do_multiply/do_mix unused; PORT_TRILOG/NOCULL/etc. branches per-vertex/per-tri) | gfx_citro3d.c:169-175,321-324; gfx_pc.c:658-682,747-754,838-862,871 | CPU waste on 268 MHz core; clutter | S | Low |
| G16 | gfx_sp_movemem ambient-light OOB read (documented in-code) | gfx_pc.c:1137-1148 | Latent, benign today | S | Low |

### M2 recommended order
1. **G1+G4 alpha combiner** — mirror the RGB if/else ladder onto `C3D_Alpha` with
   GPU_TEVOP_A_*; PRIM.a/ENV.a already packed into buf_vbo when use_alpha
   (gfx_pc.c:1072-1083). Biggest visible payoff (fades everywhere).
2. **G3 muxes** — add CC codes for ENV_ALPHA/PRIM_ALPHA/SHADE_ALPHA (broadcast .a to
   RGB in the tri packing loop :1044-1085) + CC_ONE for G_CCMUX_1.
3. **G7 texture clamp** — remove the 4096 clamp; rgba32_buf (64 KB) + sTexBuf (64 KB)
   already cover the expanded sizes; keep the existing overflow guard (gfx_citro3d.c:450-454).
4. **G5 G_BRANCH_Z + G_CULLDL** — latch w1 on G_RDPHALF_1; branch on the CC_LOD depth
   proxy; CULLDL = screen-bounds test → return. Gate behind testing (wrong predicate
   drops geometry).
5. **G10 framebuffer effects** — staged: first detect non-main color-image and redirect
   to one scratch C3D target bindable as texture (pause blur); full RTT later. High risk,
   flag-gated, do after the above.

---

## 5. D7 — TITLE/FILE-SELECT 2D UI: ROOT CAUSE FOUND (M2b)

The bypassed screens fail for **two concrete, now-identified reasons** (audit
2026-07-22). No blobs needed: **every UI static segment is a single TU** already in
gVromMap with symbols surviving gc-sections (verified in oot.elf).

### 5.1 Primary: vrom_map picked wrong base symbols
`gen-vrom-map.py` pairs each segment with a native symbol that is NOT at VROM offset 0
for at least `icon_item_static`: map uses `gInfoPanelBgDL` (0x025ffbc0) but offset-0 is
`gItemIconDekuStickTex` (0x02577ec0) — **every icon DMA reads shifted by 0x87d00** →
garbage buffers containing the observed bogus 0x02a0xxxx/0x0f082c10 words.
Verified-correct bases: parameter_static (`gHeartEmptyTex`), title_static
(`gFileSelNoFileToCopyJPNTex`). Suspect: `nintendo_rogo_static` (base is a DL);
audit all rows: icon_item_24/dungeon/gameover/nes/jpn, item_name, map_name, map_grand,
map_i, map_48x85, do_action, nes_font, message, message_texture, z_select.
**FIX:** generator picks the LOWEST-address symbol of the TU (== offset 0); regenerate
vrom_map.c; rebuild. ~1-2 h. Validates immediately: HUD item icons correct in-field
with the bypass still active.

### 5.2 Secondary: binary-image passthrough shadows segments 1/2
`PortSegmentedToVirtual`'s `[0x00100000, __end__)` native carve-out now covers
0x01xxxxxx/0x02xxxxxx because the image grew past 0x02f00000 — so seg-0x01 (title) and
seg-0x02 (parameter) references get passed through as raw .data pointers instead of
resolving to the DMA'd heap buffers (`z_file_choose.c:2056-2060`,
`z_parameter.c:3212`). **FIX:** resolve via gSegments[] FIRST when the slot is
currently SET; fall back to native passthrough only when unset. ~2-4 h incl.
re-verifying the Scene_CommandPlayerEntryList case the passthrough originally fixed.
Mirror the same rule in gfx_pc.c `seg_addr` if needed.

### 5.2c ★ D7 REAL ROOT CAUSE FOUND (2026-07-23) — heap-vs-segment-8 address collision
Deep-probed the file-select garbage DLs in gfx_pc.c `seg_addr`. The runaway DLs (`0x093cxxxx`)
are NOT branched-to directly — they START in the DMA heap (~`0x0824xxxx`) and walk ~2M steps
(16MB, the runaway-guard cap) because the DL there has no valid G_ENDDL. The bad branches:
`w1=0x08243670 → resolves to 0x0848a720`, `08243fd0→0848b080`, `08244930→0848b9e0`, ×5
(spaced 0x960). Decoded: **the file-select loads UI assets into SEGMENT 8** (DMA `-> ram=082470b0`,
so `gfx_port_segments[8]=0x082470b0`), and a DL branch `w1=0x08243670` has top nibble 8, so
`seg_addr` treats it as a seg-8 ref → `gfx_port_segments[8] + 0x243670 = 0x0848a720` (offset
2.3MB, far past the asset) → out-of-bounds garbage DL → runaway.
**ROOT: the 3DS linear heap lives at `0x08000000+`, which COLLIDES with OoT segment number 8**
(`0x08xxxxxx`). So native heap pointers in the file-select's DL data are misread as seg-8
references and re-translated to garbage. This is a fundamental address-space collision
(different from the §5.2 nibble-1/2 case). A tried small-offset-heap-resolution fix did NOT
help (culprit is a large-offset seg-8) and was reverted; the whole D7 experiment (fix +
probes + FS_TEST) is reverted — stable build clean (F62DF917).
FIX DIRECTION for next session (needs care — seg 8-D and the heap both matter in gameplay):
distinguish a native heap pointer (`>= 0x08000000`, a real DMA'd-asset address) from a genuine
seg-8..D reference. Options: (a) track the heap allocation range [heap_lo, heap_hi] and treat
`w1` in that range as native (before segment resolution) — but genuine seg-8 refs resolve INTO
that same range, so also gate on offset-vs-asset-size; (b) relocate the linear heap / DMA
buffers above `0x10000000` so they stop colliding with segment nibbles (cleanest if
linearAlloc address can be shifted, but 3DS linear heap is fixed at 0x08000000 — would need a
custom high arena for DMA'd assets); (c) have the file-select DMA UI assets into segments that
don't collide, or resolve seg 8-D refs only when offset < asset size. Verify BOTH file-select
AND gameplay (scenes/objects also DMA into 0x08xxxxxx heap and use segments) — high regression
risk. Emulator repro: force-boot FileSelect_Init (temp `#if 1` in z_opening.c + probe seg_addr).

### 5.2d ★ D7 HEAP COLLISION FIXED — file-select now runs clean, but content still black (2026-07-31)
Applied the heap-collision fix to BOTH resolvers (gfx_pc.c `seg_addr` + segmented_address.h
`PortSegmentedToVirtual`): `if (addr >= 0x08000000 && (addr & 0xFFFFFF) >= 0x100000) return
native`. Result: file-select DL runaways **43 → 0**; gameplay Hyrule Field verified clean
(0 guards/tex-skips, Link+HUD render) after a full 693-object recompile — **no regression**.
Build 9E4526AF. **BUT the file-select top screen still renders BLACK** — and I DIAGNOSED WHY (2026-07-31,
current build D8AF1AC6): it IS drawing (probed gfx_flush: mostly 2-tri texrects + one 48-tri
draw, so the 2D content submits fine) and its TEXTURES RESOLVE CORRECTLY — probed
import_texture: file-select textures are at native .data 0x025exxxx (consecutive I8 glyphs
spaced 0x180 = the font), format 0x41 = I8 grayscale, all mapped (0 tex-skips). So the content
draws from real data but is invisible. **NOT combiner, NOT resolution** (further diagnosed 2026-07-31, build A4A0D455): probed cc_id +
prim/env in gfx_sp_tri1 during FileSelect — the file-select uses `cc_id=0x01000219` (RGB =
TEXEL0, pure texture) and `0x01800800` (RGB = SHADE, the dark background quad). **0x219 is the
EXACT combiner the title demo uses to render green terrain correctly**, so the TEV mapping is
fine. And the textures are REAL: nm'd the probed addresses → `nintendo_rogo_static_Tex_*` /
`nes_font_static` (gMsgChar…) I8 glyphs — correct assets, byte-identical to the heap copies. So
the file-select draws the RIGHT textures with a WORKING combiner yet is invisible → positioning is NOT
the issue either (probed gfx_draw_rectangle 2026-07-31, build 442A221B): the file-select draws a
full-screen bg quad (0,0)-(320,240) then the logo as 16 thin 2px strips at (97,94), 192px wide,
each loading consecutive texel data (+0x180 = 192×2 I8 bytes/strip = correct strip-loading);
scissor = full 400×240. So rects are ON-SCREEN, strip layout correct, combiner correct, textures
are real assets — every pipeline stage probes correct yet output is black. REMAINING SUSPECT:
the actual TEXEL CONTENT reads as all-zero (→ alpha-test GREATER 0 discards everything → black),
OR a citro3d upload/alpha subtlety for these strip-loaded I8 textures. TEXEL DUMP DONE (2026-07-31, build 8D05E7F5):
the file-select's first ~9 texture strips (nintendo_rogo_static_Tex_000000 @ .data 0x025eec68,
+0x180 each) read **ALL-ZERO bytes**, while nintendo_rogo_static_00002A50_Tex @ 0x025f16b8 (offset
0x2A50) has real data (0x72685946…). So the file-select is drawing the EMPTY/transparent leading
strips of the Nintendo logo → alpha-test discards them → black. Two possibilities: (a) the
file-select is STUCK on the logo-intro state (never advances to file slots — likely a timer/state
value read via a segment ref that still resolves wrong; probe FileSelect's sub-state each frame),
or (b) the logo's offset-0 region is legitimately empty and the real content (0x2A50+) isn't
reached by these draws. REFINED (code-read, no build): the config progression
FileSelect_StartFadeIn(windowPosX slides to -94)→FinishFadeIn(titleAlpha→255)→CM_MAIN_MENU
(sConfigModeUpdateFuncs[] z_file_choose.c:702) is ANIMATION-driven, no segment read, so it SHOULD
reach CM_MAIN_MENU in ~15 frames. So the logo top-strips I probed are legitimately transparent,
and the open question is whether the FILE-SLOT textures (imports 11+, from title_static heap
0x0826b0b0) render once at CM_MAIN_MENU. RESOLVED THE STATE QUESTION (2026-07-31, build
C0DE00D2): probed configMode → it goes 0→1→2, so the file-select **DOES reach CM_MAIN_MENU** (state
machine + fade work; not stuck). So the file SLOTS should draw but are invisible → the seg-1
title_static file-slot textures (small-offset seg refs) resolve to wrong .data via the passthrough.
Tried "D7 part 2" (resolve ANY small-offset ref into a heap-loaded segment → heap): it did NOT fix
the slots AND it GARBLED gameplay (scene seg-2 small-offset refs are a mix of genuine refs AND
native relocated pointers — indistinguishable by offset size — so blanket-resolving them corrupts
scene geometry: red/blue streaks). REVERTED. **The correct fix must be UI-SEGMENT-SPECIFIC**: apply
the heap-resolution ONLY for the file-select's UI segments and ONLY when not in a Play/scene state
(e.g. gate on gameMode==GAMEMODE_FILE_SELECT, or only for the specific segment numbers title_static/
parameter_static use, verified against z_file_choose.c's gSPSegment calls). D7 part-1 (seg-8
large-offset→native) stays (safe, fixes runaways). COSMETIC — game fully playable via bypass. **This is a NICE-TO-HAVE (game fully playable via bypass); higher-value next
target = AUDIO (M3).** D7 resolver fix stays (correct). Ruled out this session: resolution,
combiner, positioning, scissor, strip-loading — all correct; localized to texel-content/state.

### 5.2b D7 CONFIRMED-BROKEN repro (2026-07-23, emulator, build A41F8259)
Force-booted straight to `FileSelect_Init` (temporary `#if 1` in z_opening.c, since reverted).
Result: **file-select 2D UI = 53 [GFX] RUNAWAY guards**, black top screen. It DMAs the UI
assets fine — console shows `DMA native vrom=01a02000`→heap (title_static, 0x395c0) and
`vrom=01a67000`→heap (0xc000) — but the DL references resolve to **5 garbage pointers spaced
0x960 apart: 0x093ccb20 / 093cd480 / 093cdde0 / 093ce740 / 093cf0a0** (an array of ~5
menu-item DLs). So D7 is REAL and reproduces; the vrom base fix did NOT fix it (that was the
DMA/mapping; this is the segment RESOLUTION of the UI DLs' internal refs). Root is the
§5.2 ambiguity: seg-1/seg-2 UI references vs scene native pointers, both at 0x0Yxxxxxx.
NEXT: probe gfx_pc.c `seg_addr` during file-select — log input w1 + `gSegments[seg]` + result
for refs whose result lands in [0x09000000,0x0a000000); that pins whether it's gSegments[1]
(title heap) + a bad offset, or a passthrough of a wrong .data addr. Then design the
precedence fix (resolve SET segments first vs passthrough) and re-verify BOTH file-select AND
every scene (Hyrule Field, Scene_CommandPlayerEntryList) don't regress. Core resolver = high
risk; do data-driven, test scenes after.

### 5.3 Revert steps (after both fixes confirmed)
- z_opening.c:18-42: delete the `#ifdef __3DS__` branch, restore stock
  GAMEMODE_TITLE_SCREEN + CS_INDEX_3 body.
- z_scene.c:199 unmapped-pointer guard: remove once alt-headers resolve natively.
- All UI DMAs go through DmaMgr (no bare fixed segment pointers) — dma_shim serves
  them once the map is right.

---

## 6. STABILITY — soft-locks + the memory leak (M2c)

### 6.1 Soft-lock table (audio-wait class; defuse pattern = z_message.c precedent)
| Pri | Site | Trigger | Status |
|-----|------|---------|--------|
| **1** | `ovl_kaleido_scope/z_kaleido_scope.c:4147-4154` PAUSE_MAIN_STATE_SONG_PLAYBACK | Pause → Quest Status → select learned song → A | **HARD LOCK, no exit, un-defused.** Force `ocarinaStaff->state = 0` under `__3DS__` |
| 2 | `z_message.c:3869-3960` MSGMODE_MEMORY_GAME_* | Skull Kid memory game (Lost Woods) | Un-defused; guard like the song modes |
| 3 | `z_message.c:3676-3700` MSGMODE_SCARECROW_LONG_RECORDING_ONGOING | Bonooru scarecrow recording | B exits (stall not lock); optional defuse |
| — | Combat/Z-target "freeze" | Z-target + C-up | **Ruled out as audio wait** — Z-target BGM path is fire-and-forget. Most likely = the already-defused Navi Saria's-Song prompt. If it recurs: treat as a fault, get a crash dump |
| — | River/waterfall SFX waits, metronome | various | Safe — `Audio_IsSfxPlaying` always false → advance immediately |

Already defused (keep until M3e): z_message.c:3169 (OCARINA_PLAYING), :3592
(DISPLAY_SONG_PLAYED/DEMONSTRATION), :3625 (SONG_PLAYBACK).

### 6.2 **Texture linearAlloc LEAK — top stability bug**
No `C3D_TexDelete` anywhere. gfx_pc's 512-slot cache resets pool_pos=0 when full and
re-uploads into recycled IDs (gfx_pc.c:344-352); each re-upload calls `C3D_TexInit` on
an already-initialized C3D_Tex (gfx_citro3d.c:491) **without freeing the old linear
buffer** → leak per re-upload. Texture-heavy scenes thrash the cache → linearAlloc
exhaustion → garbage/crash on long sessions. **FIX: C3D_TexDelete before re-init on
slot reuse** (small, safe), later a real LRU. Also fix `new_texture` returning 0 on
pool overflow (silent aliasing).

### 6.3 Memory budget (measured from oot.elf, 2026-07-22)
.data 35.15 MB (all native assets, resident 1:1) + .text 3.06 + .bss 1.58 ≈ **40 MB
image**; play arena 1.83 MB (z_play.c:317); VBO 2 MB (gfx_citro3d.c:817); top target
1.5 MB (6 MB if AA/wide — keep off); live textures ~8 MB steady **(forced GPU_RGBA8 =
4-8× N64 footprint, gfx_citro3d.c:491)**; ROM streamed (0 resident). Steady state
≈ 55-62 MB vs 124 MB — fine today; pressure grows with content. Latent items:
arena_shim REG_SIZE 65536 probe-spin if exceeded (arena_shim.c:17-34);
`SystemArena_GetSizes` hardcodes 64 MB free (arena_shim.c:106-110) so the game's OOM
guard is blind.

### 6.4 Performance
Already right: C3D_FrameBegin(SYNCDRAW) + conditional gspWaitForVBlank (gfx_3ds.c:128-138),
20 Hz logic via R_UPDATE_RATE, `osSetSpeedupEnable(true)` (gfx_3ds.c:54-55), 804 MHz +
L2 in RSF. Quick wins, ranked: (1) **verify the ARM build of gfx_pc.c/gfx_citro3d.c
uses -O2/-O3 -ffast-math** (the in-tree port/Makefile:10 -Os is the PC one; the 3DS
flags live in ~/bin/build-3ds.sh — check); (2) hoist the per-vertex aspect-ratio
divide (gfx_pc.c:715-717,730) to a per-frame reciprocal; (3) constant-fold `/127.0f`
(gfx_pc.c:764,785-786); (4) compile out hot-path getenv probes (G15).
Input polish: X/Y hardwired to C-up/C-down (3ds_main.c:60-75); no analog deadzone
(stick drift visible in logs: stick=(1,-2) at rest).

---

## 7. M3 AUDIO — verified plan (the long pole)

**Facts (all verified in code 2026-07-22):**
- The ENTIRE audio engine already compiles into the 3DS build — including
  `internal/seqplayer.c` and `tables/soundfont_table.c` (old "excluded" notes are
  stale; build-3ds.sh excludes only audio_stop_all_sfx.c, replaced by
  port/src/audio_stub.c). RSP ucode symbols stubbed at ultra_shims2.c:13-34.
- Tables (`gSequenceTable`, `gSoundFontTable`, `gSampleBankTable`,
  `gSequenceFontTable`) are native compiled C — NOT an endian problem.
- Frame flow to replicate: `AudioMgr_HandleRetrace` (audio_thread_manager.c:29) →
  `AudioThread_UpdateImpl` (thread.c:37) → Acmd list via `AudioSynth_Update`
  (thread.c:163) → OSTask (thread.c:178-195). **sched_shim.c does NOT "drop" audio
  tasks — it ignores non-gfx AND still acks task-done (sched_shim.c:40-42)**; that ack
  is what keeps HandleRetrace from spinning. Line ~39 is where the C mixer runs.
- ⚠ **`osAiSetNextBuffer` (src/audio/internal/os.c:33) writes N64 MMIO via
  PHYS_TO_K1 (0xA0000000|addr) — wild ARM write the moment audio is driven. Override
  it (`#ifdef __3DS__` body → `Port3ds_AudioSubmit`) BEFORE M3c.** Safe today only
  because nothing calls it.
- Blocking recvs in AudioThread_UpdateImpl (thread.c:71,96) will NOT deadlock: audio
  DMA is synchronous and self-acking (dma_shim.c:169-176). Keep both invariants.
- osTvType already NTSC at link (boot_globals.c:9). gAudioHeap = 0x38000 (224 KB,
  buffers.h:12) — small, fine.

**Endianness (the critical correction):** Audioseq (m64 bytecode) and Audiotable
(ADPCM frames) are byte-order-safe as raw BE. **Audiobank is NOT.** The plan is
in-place byte-swap during `AudioLoad_RelocateFont`/`RelocateSample`
(load.c:909-1032, 1868-1917), BUT the walk only touches offsets + gating scalars:
1. The `Sample` header word is a **packed bitfield** (unk_bit26/codec/medium/size) —
   swap the raw u32 then extract with masks manually; struct bitfields won't survive
   a naive swap.
2. The walk NEVER visits: `EnvelopePoint` s16 pairs, `AdpcmLoop` payload
   (4×u32 + s16[16] when count≠0), `AdpcmBook` payload (2×s32 + s16[order×npred×8]).
   All are read multi-byte by the mixer — must be swapped too.
3. Envelopes/books/loops are SHARED across instruments — the per-inst/per-sample
   isRelocated guards do NOT prevent double-swap. Keep a dedicated visited-pointer set.
   Helpers to write: AudioPort_SwapFontWord/SwapAdpcmBook/SwapAdpcmLoop/SwapEnvelope +
   sSwappedBlocks[].

**Mixer op set (grep-verified from synthesis.c):** aADPCMdec, aAddMixer, aClearBuffer,
aDMEMMove, aDuplicate, aEnvMixer, aEnvSetup1/2, **aFilter (6 sites — missing from the
old plan; 8-tap FIR, may stub as passthrough first)**, aInterleave, aLoadADPCM,
aLoadBuffer, aMix, aResample, aResampleZoh, aS8Dec, aSaveBuffer, aSetBuffer, aSetLoop.
Reusable from refs/sm64_3ds/src/pc/mixer.c (11, adapt DMEM addressing): ClearBuffer,
LoadBuffer, SaveBuffer, DMEMMove, SetBuffer, SetLoop, LoadADPCM, ADPCMdec, Resample,
Interleave, Mix. Net-new: aAddMixer (trivial), aDuplicate (trivial), aS8Dec (low),
aResampleZoh (low-med), aEnvSetup1/2 (low), aFilter (med), **aEnvMixer — REWRITE, not
port: OoT uses the split EnvSetup1/2+EnvMixer 4-target (dry-L/R, wet-L/R) ramp model,
not sm64's single-volume model. This is the hardest single item in the port's tail.**
DMEM constants: synthesis.c:9-19; reverb rings: synthesis.c:585-646.

**Milestones:**
- **M3a DONE** — port/src/audio_3ds.c: ndsp @32 kHz stereo PCM16, triple linearAlloc
  ring, hard-guarded on `sdmc:/3ds/dspfirm.cdc` (user must dump DSP firmware, e.g.
  DSP1 homebrew), 440 Hz boot beep, `Port3ds_AudioSubmit()` ready. Init at 3ds_main.c:226.
- **M3b (M):** `Port3ds_AudioBoot()` calling Audio_Init → AudioLoad_SetDmaHandler →
  Audio_InitSound, inserted before Graph_ThreadEntry (3ds_main.c:257); Audiobank
  byte-swap per above; osAiSetNextBuffer override. Test: boots, RelocateFont counts
  sane, AdpcmBook values small-magnitude.
- **M3c (S/M):** `Port3ds_AudioFrame()` = AudioThread_Update() once per frame; execute
  Acmd list at the sched_shim seam; temporary zero-fill mixer to prove pacing.
  Test: stable framerate, ndsp ring fed.
- **M3d (L — THE long pole):** port/src/audio_mixer.c — flat DMEM model + Acmd
  dispatch; 11 ported + 8 new ops. Staged tests: one SFX recognizable → looped
  instrument clean (validates swap) → reverb on (validates aFilter) → A/B vs emulator.
- **M3e (M):** remove Audio_Update stub (general.c:2337-2339), Audio_PortEnsureNullChannels
  (3ds_main.c:101, general.c:3160), Audio_PortInitTables (general.c:3182) +
  Audio_ResetSfx crutch, ALL §6.1 + z_message.c defuses; retest every song flow.

**Perf risk:** worst case (24-32 voices × ADPCM + resample + env + filter + reverb) on
one ARM11 core is several × the naive 8M mul-adds/s estimate. Mitigate: fixed-point
inner loops, aFilter/reverb off first, cap voices, keep Port3ds_AudioSubmit
drop-on-full (degrade, don't stall), profile with osGetTime around Port3ds_AudioFrame.

---

## 8. Milestone plan (order of work)

1. **M2c-hotfix (do FIRST, tiny):** kaleido song-playback defuse (§6.1#1) +
   C3D_TexDelete on slot reuse (§6.2). Both are crash/lock class. One build.
2. **M2b (D7):** vrom_map lowest-symbol regen + segment-priority fix (§5) → verify HUD
   icons → revert boot bypass → real title + file select. ~1-2 sessions.
3. **M2 renderer:** §4 order (alpha combiner → muxes → texture clamp → BRANCH_Z →
   blend modes; framebuffer effects last). ~3-6 sessions, one build per item.
4. **M3 audio:** §7 M3b → M3e. The long pole; can interleave with M2 items.
5. **M4 ship polish:** strip §9 list, deadzone + X/Y mapping, ARM -O2/-ffast-math
   verify + perf quick wins, icon/banner, memory headroom re-check.
6. **M5 hardening:** story playthrough; new areas may surface §3-class bugs (dungeons:
   watch framebuffer effects G10 — Lens of Truth, pause blur; bosses: effects/combiner).
7. **Phase 5 (user request): stereoscopic 3D** — no infra exists (G9); render L/R with
   IOD per mkst/sm64-port pattern; do after M4.

---

## 9. Ship strip list (complete, from audit)

**Boot.log spam emitters (game code):**
- z_scene.c:472-479 (`ALT ...`) · z_skelanime.c:1146-1177 (`SkelLink:`) ·
  graph.c:372-565 14 sites (`graph:`) · z_actor.c:967,969,2373-2378 (`Actor_Init`,
  `ActorInitCtx:`) · game.c:528-550 (`gsd:`) · main.c:110-219 10 sites ·
  z_play.c:305-546 10 sites (`Play:`) · z_player.c:3278,10656-10844 (`PIC:`/`PInit:`) ·
  z_lib.c:358 (keep the guard, drop the log).
- port: dma_shim.c:105-127 (`DMA native/ROM(BE)`) · gfx_pc.c:548,943-956,1571,1579 +
  raw fprintf probes :215,590,664-679,750,849-852,1258 · pc_gfx.c:23-69 (PC-only).
- The `#ifdef __3DS__` PortDbg extern decl blocks: game.c:521, graph.c:145,
  z_skelanime.c:1121, z_player.c:10648, main.c:46, z_play.c:288, z_lib.c:344,
  z_actor.c:942, z_scene.c:468.
- LAST: the four impls PortDbg/PortDbgX/PortLogFast/PortLogFastX in 3ds_main.c:128-148
  (+ remove the bottom-screen console or keep minimal).
- Dead code: gfx_citro3d.c do_single/do_multiply/do_mix locals + "more than 2!" printf.

**Keep (functional guards):** z_lib.c IChain runaway guard; gfx_pc.c DL-walk +
runaway + texture guards; z_scene.c:199 guard only until §5 lands.

---

## 10. Working agreements

- **Minimize the user's test cycles.** Batch independent low-risk fixes per build; one
  CIA per test; always give MD5 + what changed + exactly what to test/photo.
- Every `#ifdef __3DS__` patch gets a comment stating the N64 assumption it replaces.
- Verify fixes in the binary (arm-none-eabi-objdump/nm via WSL script files).
- Probes: self-limiting, labeled `[TAG]`, removed after they answer.
- Update this document + Claude project memory after every hardware-verified result.
- Never `git checkout` decomp files. Never ship per-frame logging.

# 60 fps on Old and New 3DS — research and plan

Goal: 60 rendered frames per second on both the New 3DS and the Old 3DS, with the game's logic still
running at its designed 20 updates/s (N64-identical gameplay). Status 2026-09-30: the game holds
19.6–19.7 updates/s on a New 3DS, i.e. full N64 speed at 20 fps.

## Why this is an architecture problem, not a speed problem

- OoT's logic advances once every 3 N64 video retraces (R_UPDATE_RATE = 3, 20 Hz). Physics, animation,
  timers and cutscene scripts all assume that step. Running the logic 3× as often makes the whole game
  3× faster. 60 fps therefore means *interpolated* frames: 2 extra frames drawn between logic updates,
  with every object's transform blended between the previous and current update. Ship of Harkinian
  (SoH) does this on PC.
- The 3DS CPU is far faster than the N64's (New 3DS: 4 × 804 MHz ARM11 vs 93.75 MHz VR4300), but
  this port currently does on the CPU what the N64 did on its RSP co-processor: display-list
  interpretation, vertex transform, lighting, texgen, fog and clipping (`gfx_pc.c`, a Fast3D
  interpreter). That costs 16–25 ms per frame on a New 3DS. Measured hardware breakdown, v10, busy
  scenes:

  | Stage | Time per frame |
  |---|---|
  | Game logic + display-list build | 2–6 ms |
  | Display-list interpretation (CPU) | 16–25 ms |
  | GPU wait | 10–14 ms |
  | Audio (main thread) | 1.5–5 ms, mostly waiting for the audio worker |

  Interpolated rendering the naive way re-runs the whole display list for every drawn frame:
  3 × 20 ms = 60 ms of work per 50 ms logic frame. That doesn't fit even on the New 3DS, and the Old
  3DS CPU is ~3× slower (268 MHz, no L2 cache).
- **Conclusion:** 60 fps on both consoles needs the per-frame cost moved off the CPU. The idea is to
  interpret each logic frame's display list once, and let the GPU re-draw it three times with
  interpolated matrices.

## Target architecture

1. **GPU vertex transform.** The PICA200 vertex shader applies the N64 modelview × projection, the
   lighting (N64 directional + ambient lights), texgen, and the fog factor, from uniforms. The CPU
   uploads raw N64 vertices (converted once per vertex buffer, not per triangle). This removes most
   of `gfx_sp_vertex` / `gfx_sp_tri1` CPU work. The PICA can't cull or clip per N64 rules on its own,
   so CPU-side work stays only where the game needs it (G_CULLDL bounding volumes, the few CPU reads).
2. **Recorded frames.** Interpreting a logic frame's display list produces a list of GPU draws: vertex
   buffer range, state, and a *matrix slot* per draw instead of baked positions. Drawing a frame means
   replaying that list: cheap citro3d state + draw calls, no interpretation.
3. **Interpolation.** Each matrix slot is matched with the same slot of the previous logic frame.
   Drawn frames at t = 0, 1/3 and 2/3 use lerped (translation) / slerped (rotation) matrices; the
   camera is interpolated the same way. Matching strategy, in order of robustness:
   a. game-side tags: `Matrix_*` calls and `MATRIX_FINALIZE` sites carry an id (actor pointer +
      limb index + call site), like SoH's `FrameInterpolation_RecordOpenChild`;
   b. fallback: display-list position plus the called DL's address.
   Unmatched slots (spawns, culled-in) are drawn un-interpolated.
4. **Pacing.** Present on every retrace (60 Hz). Run the logic update every 3rd retrace, on a second
   core where possible (New 3DS: 4 cores; Old 3DS: the syscore can take audio).

## Phases (each ends with a measurable gate)

| Phase | Work | Gate |
|---|---|---|
| P0 Measure | Per-stage timing build (`PORT_EXTRA=-DPORT_PERF_STAGES`). An **Old 3DS simulation** switch on New 3DS hardware (`osSetSpeedupEnable(false)`: 268 MHz, no L2), since only a New 3DS is available. | Breakdown per stage on both speeds |
| P1 Cheap wins | Audio fully asynchronous (no main-thread wait, ~4.5 ms). Pacing fix (done in v10). Batching: fewer draw calls / state changes. | N3DS steady 20; O3DS-sim ≥ 20 |
| P2 GPU transform | Vertex shader does MV×P, lights, texgen, fog. CPU keeps G_CULLDL and CPU-visible results. Verify with `tools/statediff` fbdiff (must not regress vs N64). | DL CPU ≤ 6 ms/frame on N3DS |
| P3 Recorded frames | Draw list per logic frame, replayed per drawn frame. | Replay ≤ 3 ms CPU |
| P4 Interpolation | Matrix-slot matching (game-side ids), camera interpolation, 60 Hz presentation. | 60 fps N3DS; image identical to N64 at t=0 frames |
| P5 Old 3DS | Tune O3DS-sim until 60. Fallback: 30 (t = 0, 1/2), selectable. | O3DS-sim 60 (or a 30 option) |

## Risks

- **GPU fill rate.** The New 3DS 800×480 AA mode costs 10–14 ms of GPU per frame. At 60 fps the GPU
  budget is 16.7 ms per frame. Options: 400×240 without AA when interpolating (the Old 3DS has no wide
  mode anyway).
- **Effects that read back the framebuffer or depth** (pause background, lens flare, Navi glow): these
  are once per logic frame, and are fine.
- **Interpolation artifacts** at teleports/cuts: detect big jumps and skip interpolation for that
  slot, as SoH does.

## Progress log

### 2026-09-30

**P0 (measure):** `o3ds_sim`, `perf_stages` and `perf_ab` settings are shipped. `perf_ab=1` alternates
N3DS / O3DS-sim speed every 4 reports; each report is tagged "perf mode ...".

**Hardware (v11, perf_stages on, N3DS), display-list CPU 20–35 ms per frame:**

| Stage | Time per frame |
|---|---|
| Triangle setup | 6–11 ms |
| Flush | ~3 ms |
| Vertex transform | 1.6–3.3 ms |
| Textures | 0.5–1.4 ms |
| Not attributed (DL walk, other commands, timer overhead) | 10–18 ms |

**Command mix** (Azahar, title demo; counts are exact): 5,728 DL commands per frame for 1,415 triangles
and 201 draws.

| Command | Per frame |
|---|---|
| TRI2 | 799 |
| SETTILE | 684 |
| PIPESYNC | 675 |
| LOADSYNC | 384 |
| SETTIMG | 384 |
| SETTILESIZE | 376 |
| VTX | 257 |
| LOADBLOCK | 248 |

Texture setup and state commands dominate, so per-command overhead is the target.

**P1 wins so far (render output verified identical by fbdiff, boot_title):**
- `PortMem_ReadableEnd` caches confirmed ranges: 153 → 0 `svcQueryMemory` kernel calls per frame.
- CI palette hashes are computed once per TLUT load instead of on every CI texture lookup (was 256
  multiply-xors per CI8 lookup).

**Hardware A/B (v16, `perf_ab=1`):**

| Mode | Updates/s | Display list | Audio on the main thread | Microcode per task |
|---|---|---|---|---|
| N3DS | 19.1–20.2 | 20–32 ms | 4–5 ms | 1.8–2.5 ms |
| O3DS-sim | 7.7–10.5 | 48–73 ms | 23–39 ms | 5.3–6.0 ms (≈ 34% of a core at 60 tasks/s) |

**P1 audio (v17):**
- Audio pumps run *during* display-list interpretation (every 256 commands, one per elapsed retrace),
  the way the N64 audio thread preempts rendering. The worker's microcode now overlaps the frame
  instead of back-to-back catch-up waits after it.
- On the Old 3DS, the syscore time limit is 55%, falling back to 30%. Stock firmware allows 30%, Luma
  89%; 80% has been reported to hang the Rosalina menu.
- ARMv6 SIMD in the microcode: SSAT clamps, SMLALD for the FILTER FIR, QADD16 for ENVMIXER
  accumulation. A startup self-test compares them against the scalar code on random data:
  0 mismatches.

  | Op (Azahar, us/frame) | Before | After |
  |---|---|---|
  | ENVMIXER | 5767 | 3822 |
  | FILTER | 4680 | 3482 |
  | ADPCM | 3341 | 3132 |
  | RESAMPLE | 2323 | 2106 |
  | MIXER | 1303 | 1040 |

  Total −23%.

**Hardware per-opcode profile (v18, `perf_stages` + `perf_ab`), ms per frame:**

| Mode | DL total | TRI2 + QUAD | TEXRECT | VTX | All state/texture commands |
|---|---|---|---|---|---|
| O3DS-sim | 75–79 | 30–33 | 7–8 | 3.5 | < 5 |
| N3DS | 27–51 | 8–17 | 0.4–2 | 1–3.6 | < 2 |

**Triangle processing dominates:** about 16 µs (~4,300 cycles) per triangle at 268 MHz. The next P1
targets are in `gfx_sp_tri1_impl`:
1. Per-vertex UV/color conversion is recomputed for every triangle sharing a vertex.
2. Double vertex copy (gfx_pc packs, then gfx_citro3d re-swizzles into the VBO).
3. State re-evaluation.

GPU transform (P2) remains the structural fix.

**v20 (P1, verified in Azahar: all 101 child-tour scenes unchanged vs the N64 reference, avg 5.92):**
- **Indexed batches.** Each vertex is written once per batch straight into the VBO in the final PICA
  layout; loaded N64 vertices are reused by index across the triangles sharing them. Triangles push 3
  u16 indices and batches draw with `C3D_DrawElements`. This removed the per-triangle 39-float staging
  write and the backend's copy/re-arrange pass.
- **Division-free backface culling:** the sign of the homogeneous determinant det(x, y, w).
- **Per-loaded-vertex packed-vertex cache.** Correct but no measurable gain in Azahar; kept.
- **VBO/index buffer data-cache flush before `C3D_FrameEnd`.** A hardware correctness fix: the GPU could
  read stale vertices from the CPU cache.

**Note:** Azahar's system tick doesn't model the 3DS CPU (the title demo shows ~18 ms in Azahar vs
27–51 ms on a New 3DS). CPU gains must be measured on hardware (`perf_ab` + `perf_stages`).

### 2026-09-30 (later): frame interpolation implemented (P3), verified in Azahar

**Design, taken from the two working references:**
- Zelda64Recomp (Majora's Mask, RT64): the game emits `gEXMatrixGroup(id, ...)` around every transform
  it wants interpolated (patches/actor_transform_tagging.c, sky_/camera_/effect_transform_tagging.c,
  ids in transform_ids.h). RT64 pairs matrices by id, interpolates them DECOMPOSED, blends skin vertices,
  and skips a group on camera cuts (camera_transform_tagging.c heuristics) or teleported/new actors.
- Ship of Harkinian (OoT): `FrameInterpolation_RecordOpenChild(actor, limb)` labels; the renderer swaps
  each Mtx for the interpolated one; angle jumps > 90 degrees are not blended.
- First attempt here guessed identity in the renderer (next static DL + occurrence): wrong pairings
  (same model drawn twice, order swapped between frames), camera keyed differently from the sky -> the
  wobble and sky flicker seen in the emulator. Replaced by game-side tags.

**Implementation:**
- `include/port_interp.h`: G_NOOP tags (`w0 = G_NOOP | 0x6E << 16 | op << 8 | flags`, `w1 = id`),
  push/pop, flags SKIP (record, don't blend) and VERTS (blend CPU-written vertex positions).
- Tag sites: Actor_Draw (actor instance; SKIP when it moved > 300 units since prevPos), all 12
  SkelAnime limb draw functions (limb index, opa+xlu), Skin_DrawImpl (per skin limb, VERTS for animated
  limbs: Epona), View_ApplyPerspective (camera; cut = eye/at jump > 300 or eye velocity change > 100),
  Skybox_Draw (SKIP on camera cut).
- `gfx_pc.c`: group stack with nested ids; matrix key = group path + index in group; tables recorded by
  the exact pass; rigid matrices interpolated decomposed (translation lerp, per-axis scale lerp,
  nlerp + Gram-Schmidt rotation, > 90 degrees = snap); perspective per element; untagged = as is.
  No translation threshold in the renderer: limb matrices are in unscaled model units (x100), a gallop
  step looked like a 2000-unit teleport.
- `3ds_main.c`: per logic frame, n = in-between passes that fit (`elapsed + (n+1) * pass <= budget`),
  each presented on its own retrace, then the exact pass. `fps60=0` off, `fps60=2` forces passes (emulator
  verification only - the game slows down). In-between passes skip readbacks/captures.
- Verification: `sdmc:/3ds/oot/capture_interp` dumps logic frames 198/199 (+200k) all passes; the step
  sizes between consecutive frames are even (e.g. 7.9 7.7 7.9 7.7 7.7; leg region 37 38 41 around a 43
  boundary step) - before the tags the boundary step was 60 vs 24 in-between.

**Status:** correct, but passes cost a full display-list walk (Azahar ~22 ms emulated; hardware DL 27-51 ms
on New 3DS), so in default adaptive mode they rarely fit yet. 60 fps now depends on pass cost: P2 (GPU
transform: vertices uploaded once, per-pass matrix uniforms) is the structural fix. Not tagged yet
(still 20 Hz): EffectSs particles, sword trails, lens flare, billboards outside actors.

### 2026-09-30 (night): replay - in-between frames without re-walking the display list (v21)

- The walk records instead of drawing: `gfx_citro3d.c` logs state calls/draws (`OP_*`, texture uploads run
  normally); `gfx_pc.c` keeps per VBO vertex a recipe (source vertex: object-space position at t = 1/3
  and 2/3 + matrix slot + clip adjustments; split/clipped vertices: weights of the original triangle's
  three vertices, solved in clip x/y/w) and runs the matrix stacks at t = 1/3 and 2/3 alongside the real one.
- Each shown frame: `C3D_FrameBegin` (waits for the GPU), rewrite VBO positions (+ stereo offset
  s' = s + dw), re-issue the log, `C3D_FrameEnd`. Colours/lighting/UVs stay those of the logic frame.
- Fallbacks: an off-screen render target mid-walk (pause menu, PreRender) replays what was recorded and
  draws the rest directly (no in-between frames that logic frame); readback/capture frames skip recording.
- Verified (Azahar): replay frames step exactly like the earlier full re-walk passes (9.3 9.6 11.4 10.8
  10.5); gameplay + pause menu smoke: no crash, 0 UnmappedAccess; Azahar emulated cost walk ~34-50 ms,
  replay ~10 ms (hardware numbers pending: `perf us/walk+first frame`, `perf us/replay frame`,
  `perf frames shown/s x10`).

### 2026-10-01: hardware v23 → v24

- **v23 (vsync fix: `C3D_FrameBegin(0)` instead of `C3D_FRAME_SYNCDRAW`, which waited for the next vblank on
  both screens on top of our pacing):** New 3DS 42–47 frames/s in light scenes (v21: 22–32), 21–25 heavy;
  Old 3DS 10 updates/s (v21: 8–9). Renderer + audio microcode built `-O3 -ffast-math` (HOT_OBJS).
- **Sampling profiler (`prof=1`, port_prof.h):** one-byte stage markers, sampler thread on core 1 every 250 us.
  Old 3DS shares: tri emit 17–25%, swap 9–13%, vtx 11–13%, game 7–9%, audio 8–9%, tri setup 7–9%, mtx 5–7%,
  replay/submission 5–7%, input 2–7%. New 3DS heavy: swap 15–24%.
- **v24:** depth readback queued inside the frame (no CPU stall; waited via `C3Di_RenderQueueWaitDone` at the
  game's first depth read); `C3D_FrameEnd(GX_CMDLIST_FLUSH)` (no whole-linear-heap cache flush; GPU-read
  buffers are flushed where written); minimap parchment + map texture cached (markers only per redraw).
- Status report doc: https://claude.ai/code/artifact/5eff6a48-a625-45ec-a3eb-db9e07a1cc82

### 2026-10-01: emulator benchmark, GPU vertex path (gpu_vtx=1, off by default)

**Method.** `tools/perfbench.sh LABEL [secs] [settings...]`: title attract demo (same content every boot),
sampling profiler on, averages the perf reports. Azahar's clock advances per executed instruction, so the
numbers are an instruction-count proxy (hardware is 1.5–2.8× slower: cache misses, VFP latency). Repeat
runs agree within 0.1% (16.318 / 16.337 ms). `perf_stages` distorts even in Azahar (39 vs 16 ms): not used.

| Display-list CPU per frame (Azahar, fps60=0) | ms |
|---|---|
| CPU path, baseline | 16.33 |
| + per-vertex divisions hoisted, alpha/stencil state cached | 15.44 (later 16.1 after restructuring; vertex loop split) |
| GPU path, first version (palette per draw, no CPU rejection) | 24.5 (367 draws vs 144) |
| GPU path + persistent palette + lean triangle path | 22.1 |
| GPU path + per-load bounding-box frustum rejection | **14.1** (179 draws, emit 68 → 27 ‰, build 9 → 0 ‰) |

**GPU path design.** `shader_gpu.v.pica` (second DVLE in shader.shbin): model-space vertices (struct GpuVtx,
48 bytes: pos, uv0, uv1, skinned delta, shade bytes, palette index), a 20-matrix palette in uniforms (rows
already produce the portrait output, half-pixel offset and aspect squeeze), fog and the stereo offset in the
shader, cull mode per draw (`C3D_CullFace`). The CPU keeps lighting/texgen/texture coordinates. Each G_VTX
load's model-space bounding box is transformed (8 corners) for exact off-screen rejection: a material
entirely off screen was otherwise still a draw. 60 fps replays re-issue the recorded draws with the palette
matrices of t = 1/3, 2/3 (no VBO rewrite); skinned vertices blend via the delta attribute.

**Findings.** The per-vertex matrix multiply was NOT the vertex stage's main cost (vertex time barely moved);
lighting is ~1/3 of it, the rest is bookkeeping. citro3d re-checks every uniform's dirty flag on each draw.
Title-screen comparison vs the N64 (bootflow, 6 frames): identical to the CPU path (0.9–7.0 mean error).

**Depth readback, lazy (default).** The game reads depth (Navi's glow, lens flare) in its next update; the copy
now runs at that first read (the v23 synchronous call, outside any frame) instead of right after FrameEnd,
where it waited for the whole GPU frame (hardware v23: "swap" 15–24% of New 3DS time). Updates that read no
depth copy nothing. statediff builds keep the eager per-frame copy. The asynchronous in-frame variant
(v24/v25, hardware freezes) is removed.

### 2026-10-01 (afternoon): splitting is the triangle cost; GPU path fallback; hardware A/B switch (v27)

- **Where triangle time goes.** With a dedicated profiler stage (`prof tri split`), the N64-exact splitting
  of big near triangles (screen-linear shade/fog, near-plane clamp, guard-band clip) is most of the
  triangle cost on both paths: in the GPU path, the ~215–248 triangles routed to the CPU splitter cost as
  much as the CPU path's whole emit stage.
- **GPU path fallback.** Triangles needing N64-exact setup (vertex behind the eye, in front of the near
  plane, or a big depth range on an edge ≥ 10 px) are rebuilt in clip space and sent through the CPU
  split code, after the same off-screen rejection and face culling the CPU path does; their pieces are
  drawn through the identity palette entry with fog precomputed (vertex `dpos.w = 1`). The vertex format
  carries a 4-component position for that (56 bytes).
- **Shading-aware split skip (both paths).** When the three vertices' shade and fog differ by at most
  2.5/255, perspective and screen-linear interpolation give the same values, so the triangle is not split.
  CPU path 16.1 → 15.6 ms (Azahar); the 101-scene N64 comparison is unchanged (avg 5.92).
- **Texture binds cached** (a bound unit is not re-bound; re-upload and sampler changes re-bind): no
  measurable gain in Azahar, kept.
- **Azahar numbers (title demo, display-list CPU):** CPU path 15.6 ms; GPU path 17.4 ms. The GPU path
  removes mostly float work, which the ARM11's VFP makes far more expensive on hardware than Azahar's
  per-instruction clock suggests, and its 60 fps replays are much cheaper: the decision is the hardware A/B.
- **`gpu_ab=1`** switches the vertex path between frames every 2 perf reports (shader program, vertex
  format and uniforms swapped by `gfx_citro3d_set_gpu_mode`); with `perf_ab=1` (console speed every 4
  reports) one session measures all four combinations. Each report is labelled `perf vertex path CPU/GPU`.
- **Environment.** A fresh clone needs: ROM → `baseroms/ntsc-1.0/`, `gmake setup`, the decomp build
  (`gmake VERSION=ntsc-1.0`) for generated sources, conda off PATH for tools/audio; statediff runs with
  `.venv/bin/python3`. The N64 build had broken (port_interp.h outside include/, a C89 statement in qrand.c):
  fixed.

### 2026-10-02: present gate, displayed-frame estimate, adaptive splits

- **Where a New 3DS update goes (hardware v34, GPU path, profiler; the 3D slider was up in every gameplay report,
  so each draw was issued for both eyes - 2D costs less, and reports now log `frames drawn in 3D`):** game logic 6 ms, display-list walk ~18.7 ms,
  each `C3D_FrameEnd` ~2.2 ms (whole linear-heap cache flush), each replay ~2.4 ms of CPU plus a ~5 ms wait in
  `C3D_FrameBegin` for the previous frame's GPU work (GPU wait 11-14 ms per update, 27% of the profile). citro3d
  has one command buffer, so a frame's CPU work cannot overlap the previous frame's GPU work: the chain
  logic → walk → frame end → GPU → replay → frame end → GPU → replay is ~48 ms of the 50 ms update.
- **Frames were lost, not just late.** citro3d swaps a frame's buffers when its GPU work ends and the LCD takes
  them at the next vblank: the logic frame finished ~27 ms into the update, after the first in-between frame's
  refresh, and the replays followed back to back, so two frames often finished within one refresh. The first
  was never shown, and the second's display transfer wrote into the buffer being scanned out. `frames shown/s`
  counted frames drawn; the report now also estimates `frames displayed/s` and `frames replaced before shown`
  (GPU finish time from `C3D_GetDrawingTime`, vblanks from a 59.831 Hz clock anchored at `gspWaitForVBlank`).
- **Present gate (`present_gate`, default on):** a frame is handed to the GPU only after the vblank that shows the
  previous one, so every frame gets its own refresh. It exposed a pacing bug: `osGetTime` has 1 ms steps, so an
  update ending 50.4 ms after its start read as 50 < 50.14 and waited one more retrace. Pacing now uses
  `svcGetSystemTick`, and an update that ends within 3 ms of a vblank starts on it. Gate waits are kept out of
  the walk/replay cost averages that choose the number of in-between frames.
- **Azahar (title demo, GPU path), gate on / off:** frames displayed 58.5-59.7 / 55.7-58.3 per second, frames
  replaced before shown 0 / 16-52 per report, updates 19.5-19.9 / 19.7-19.9 per second. At Old 3DS speed (CPU
  25%, frame skip): 19.9-20.2 / 18.8-18.9 updates per second.
- **Adaptive coarse splits on New 3DS (`split_auto`, default on):** with the gate, an update's frames take three
  consecutive refreshes, the logic frame by the second after the update starts, so logic + walk + GPU must stay
  under ~32.9 ms (v34: ~33.5). The coarse split thresholds (Old 3DS) save ~3 ms; they are now also used while
  the measured margin to that deadline is under 1.5 ms (back to fine above 6 ms). The report logs the logic
  frame's finish time and the updates that used the coarse thresholds. Statediff builds never choose in-between
  frames, so their comparisons keep the fine thresholds.
- **Next (hardware):** `present_ab=1` session (displayed frames, replaced frames, gate wait, update rate on a New
  3DS); then shorten the chain: frame-end flush (`cmdlist_flush`, v38 A/B), walk CPU (packing, submits).

### 2026-10-02 (evening): hardware v39 - the GPU is the bottleneck

- **GPU time per frame (C3D_GetDrawingTime, New 3DS, 2D, AA on, widescreen): 9.4-11.8 ms in gameplay, 8.8 ms on
  the file select.** In 3D (no AA: two 240x400 eyes) 8.9 ms. 2D gameplay profile: GPU wait 34-37% of the time,
  game logic 17%, the whole display-list walk and replays ~27%. Three frames per update need ~33 ms of GPU,
  serialized with the CPU work by citro3d's single command buffer.
- **The frame-end cache flush is cheap:** `C3D_FrameEnd` 123-162 us with the whole-heap flush, 23-26 us with
  `GX_CMDLIST_FLUSH` (no freeze in either). Not worth a default change for now.
- **The present gate hurt in this regime:** frames displayed 37.6/s with it vs 43-48 without (69-74 frames
  replaced per report). With frames spaced one per refresh, the last one reaches the GPU at the update boundary,
  and the game's depth read (Navi's glow, every update) waited for it: game logic 8.9 -> 14.4 ms; the logic
  frame then finished 40-43 ms after the update's start (deadline ~33), so one in-between frame was chosen.
- **The v34 AA test was invalid:** that session ran in 3D, where AA is not used.
- **v40:** early depth copy (the logic frame's first shown frame is copied before the next frame starts, where
  C3D_FrameBegin waits anyway); `aa_ab=1` + `present_ab=1` in 2D (all four combinations every 8 reports);
  report fix (`dl (cpu)` no longer subtracts the in-between frames' waits).
- **Next:** if AA off brings the GPU near 6 ms per frame, the gate's schedule fits (logic frame ~28 ms) and 60 fps
  is within reach; else smaller texture formats (RGBA8 everywhere today), textures in VRAM, and overlapping CPU
  work with the GPU.

### 2026-10-03: hardware v41, anti-aliasing off, flip presenter (v43)

- **v41 (New 3DS, 2D):** anti-aliasing more than doubled the GPU's work: 11.6 ms per frame with it, 4.6-5.0 without;
  with the present gate, 37.5 vs 52.6-53.8 frames displayed per second (0 lost). AA is now off by default (`aa=1`).
  3D (never anti-aliased, two 400x240 eyes): GPU 6.7-8.4 ms per frame, 44-56 frames displayed.
- **What remained:** the game thread waited ~15 ms per update for refreshes (the gate before each in-between frame
  and pacing), and an update's first frame had to be drawn within two refreshes of the update's start (it took
  ~30-33 ms), so heavier updates stretched to four refreshes (18.5-19.2 updates per second).
- **Flip presenter (gfx_3ds.c, `flip`, default on):** every frame is copied by the GPU into its own buffer of an
  8-buffer ring (a display transfer queued right after the frame's commands), then the GPU stamps the frame's number
  into a VRAM word (memory fill). A small thread wakes 1.5 ms before each vblank and points the LCD at the newest
  finished frame due by then (`gspPresentBuffer`, latched at the vblank). The game thread draws an update's three
  frames back to back and never waits for a refresh; the frames are due on the update's 4th, 5th and 6th vblank, so
  the first one has a whole update period to be drawn (one refresh more latency than before). A late frame repeats
  the previous one for a refresh, frames are never lost or torn, and the report counts frames actually shown.
- **Azahar:** 2D 59.2-59.5 frames shown per second at 19.8 updates per second (0-1 late frames per report); 3D on/off
  every 2 s (`stereo_test=1`) works; Old 3DS speed (CPU 25%, frame skip) 18.9-20.1 updates per second; the 7-scene
  N64 comparison is unchanged. `flip=0` returns to citro3d's own output (and the present gate).

### 2026-10-03 (night): hardware v42 read-out, CPU/GPU overlap (v46)

- **v42 (New 3DS):** 2D 54.6-56 frames shown per second. 3D reports were not CPU-bound: the game thread idled
  33-34% of the time in the present gate (gone with the flip presenter, v43). Old 3DS speed: logic + audio ~16 ms per
  update, ~54 ms of renderer CPU per drawn frame, so frame skip sits at its floor (9-10 shown/s, same as v30/v34). The
  half-second pauses were the perf report's SD writes: log lines now go through a background writer thread.
- **The remaining serialization:** every frame was its own citro3d frame, and `C3D_FrameBegin` waits for the GPU to
  finish the previous one (one command buffer, GX queue cleared there). An update was logic + walk + 3 x (GPU) + 2 x
  (replay CPU) in sequence; in 3D on hardware about 6 + 19 + 3 x 8 + 2 x 6 = 61 ms of a 50 ms update.
- **Overlap (`overlap`, default on; gfx_3ds.c):** with the GPU vertex path an in-between frame only re-issues the walk's
  draws with other palette matrices (uniforms in the command stream), so the update's frames now share one citro3d
  frame. Each frame is split off with `GX_CMDLIST_FLUSH` (unflushed splits froze hardware in v24/v25), the linear heap
  is flushed once (as `C3D_FrameEnd(0)` did), the GX queue is started at once, and the next frame's commands are built
  while the GPU draws. The early depth copy is queued after the walk's frame and waited for (queue entry index) at the
  game's read: inside a citro3d frame `C3D_SyncDisplayTransfer` splits without the flush and does not wait. The command
  buffer is 1 MB (overflow is a panic in libctru): peak use 21.5% in 2D and 36.5% with 3D on, for a whole update.
- **Azahar:** 59.8 frames shown per second, 2 overlapped frames per update, 0 replay fallbacks, 3D toggling every 2 s
  works, 7-scene N64 comparison unchanged. Azahar's GPU runs synchronously, so the gain is hardware-only: expected up to
  ~12 ms per update in 3D and ~6 in 2D. `overlap_ab=1` alternates it every 2 reports for the hardware comparison.
- **Next:** the second submission of every draw in 3D (a command list reused for both eyes), then the Old 3DS renderer
  (static room geometry reused between frames).

### 2026-10-03 (evening): v47 hardware, Old 3DS profile, render thread (v49)

- **v47 (New 3DS):** 2D 59.7-59.8 frames shown per second with the overlap on (58.2 / 53.3 with it off), 3D 53-58 with
  dips in big views (Kokiri Forest from above); the overlap is now always on. Old 3DS speed: full game speed, 9.4-11
  frames shown (frame skip every other update): logic + audio ~13 ms per update, renderer ~38 ms per drawn frame.
- **Renderer CPU -16% to -22% (v48):** measured with `tools/pcprof.py` (Azahar GDB-stub PC sampling), see
  docs/3ds-native-speed-research.md. On the Old 3DS this should make most updates fit in 50 ms (no frame skip).
- **Render thread (`render_thread`, off by default; `render_thread_ab=1` alternates every 2 reports):** the game thread
  hands each update's display list to a thread on core 2 (New 3DS) and goes on with the next update's logic, the
  N64's CPU/RCP split. One update in flight (the next hand-over waits for the renderer), game requests latched per
  frame (pause capture, menu stereo flag, Link's distance), texture-cache invalidations from the game thread queued,
  depth read only from queued copies, audio pumped only by the game thread, HOME / sleep handled on the game thread
  while the renderer is idle. Azahar: 7-scene N64 comparison unchanged with it on, pause capture correct, 3D and Old
  3DS paths run; Azahar runs all emulated cores on one host thread, so the gain is hardware-only. Not used on the
  Old 3DS yet (its second core is shared with the system and the audio mixer).

### 2026-10-03 (night): v49 hardware, the Old 3DS layout (v50)

- **v49, render thread A/B:** New 3DS 2D 59.7-59.8 shown with and without it; 3D with it up to 59.8 (a report entirely
  in 3D), 56-58 otherwise. At Old 3DS speed (on core 2, which a real Old 3DS does not have): 58.4 frames shown in a
  light scene (1,130 triangles, walk 24.8 ms), ~20 in a heavy one (2,169 triangles, walk 43.6 ms); without it ~10
  (frame skip every other update; the walk's time also includes the audio pumps made during it). Now on by default.
- **Old 3DS layout (v50):** core 1 gives an application at most 55% safely (stock firmware 30%), too little for the
  drawing but enough for the game logic and the audio engine (~13 ms per update at 268 MHz). The game loop moves to a
  thread on core 1; the main thread (core 0) waits for the frames it hands over and draws them, with the audio mixer
  beside it. Expected on a real Old 3DS: about 39 ms of drawing per update, enough for 60 frames in light scenes.
  `o3ds_layout=1` forces it on a New 3DS, with `o3ds_sim=1` (268 MHz, no L2) for a faithful Old 3DS test. Azahar:
  7-scene N64 comparison unchanged in this layout.

### 2026-10-03 (late): v50 on hardware, Old 3DS layout corrected (v51)

- **v50 (game on core 1, drawing on core 0), Old 3DS speed:** low frame rate and crackling audio from the title
  screen (41 instead of 60 updates per second, 57.6 audio pumps per second instead of 60): at 55% of core 1 the game
  thread could not keep up. Turning 3D off froze the game (no crash dump; the log stopped at `[stereo] leave`, the
  low-priority log writer shared core 0 with the busy renderer).
- **v51:** the game and all of the audio stay on core 0 (~27 ms of 50 per update at 268 MHz); only the drawing goes
  to core 1 (`o3ds_layout=1` on a New 3DS). The game thread pumps audio while it waits for a slow frame. Azahar at
  25% CPU: 18.9 updates and 21.6 frames shown per second, 60.2 audio pumps per second (single core: ~11 shown).
  Watchdog: if the renderer has not finished a frame for 2 s, `boot.log` gets `[render] STUCK ... phase N` (1 job
  start, 2 frame start, 3 walk, 10+i in-between frame, 20 closing, 30 presenter ring full, 31 C3D_FrameBegin,
  32 3D switch, 33 C3D_FrameEnd).

### 2026-10-04: v51 on hardware: the Old 3DS's second core cannot draw (v52)

- **v51 (drawing on core 1, 55% requested), Old 3DS speed:** 72-128 ms of drawing per frame for 745-1,118
  triangles (25-40 ms on core 0), the game waiting 70-122 ms per update for it: 7-13 updates per second. With v50's
  result (the game loop there could not keep up), the Old 3DS's second core is out for both: the system keeps most of
  it. Only the audio mixer (~22% of a core at 268 MHz) stays there.
- **v52:** Old 3DS (and `o3ds_layout=1`): no render thread, the mixer on core 1 - the v49 single-core path, now also a
  faithful audio test on a New 3DS. New 3DS unchanged (drawing on core 2).
- **What 60 fps on the Old 3DS needs:** everything on one 268 MHz core: logic ~9 ms + audio engine ~4 ms per update
  leave ~37 ms for three frames, against ~25-40 ms for one today. The translation of N64 display lists has to get
  about three times cheaper: static geometry converted once (vertex/index buffers and draw lists reused across
  frames), lighting and texture-coordinate generation in the vertex shader, and in-between frames as re-submissions
  of recorded GPU command lists.

### 2026-10-04: raw vertex path on by default, and what the Old 3DS really spends (v53)

- **Correction to the Old 3DS numbers above:** the report's per-frame times (`perf us/frame dl (cpu)`, game, triangles,
  draws) are averaged over all 300 updates of a report, skipped updates included. With frame skip drawing every other
  update (v49 at Old 3DS speed, single core: 145-149 of 300 skipped), a drawn frame cost ~75-80 ms, not 37-40.
- **Where it goes on hardware** (v49 sampling profiler, Old 3DS speed, single core, share of all time): writing
  GPU-path vertices into the vertex buffer 12-17% (1% at New 3DS speed), citro3d draw submission 8-10%, game logic
  14-19%, triangle setup 7%, display-list walk 6-7%, vertex transform + lighting 11%, shading split 7%, matrices 5%,
  audio 6-7%. The vertex buffer and command buffer are linear memory; without an L2 cache their writes cost far
  more than the instruction count suggests (Azahar shows the vertex writes as 2.5%).
- **v53:** the raw vertex path is on by default (`raw_vtx`): most vertices go to the GPU as the game's own 16 bytes
  instead of 56 CPU-prepared bytes, and the shader does the N64's per-vertex work. Loads near the camera or deeper
  than 3:1 stay on the CPU path, which keeps the N64's screen-linear shading (7-scene comparison unchanged). Azahar,
  identical workload: renderer 13.05 -> 11.10 ms per frame (Old 3DS path); at 60 fps the logic frame gets cheaper
  (20.8 -> 18.7 ms) but the two in-between replays dearer (2.0 -> 2.8 ms each: more draws, because each lit limb
  needs its own light uniforms), so an update is only ~2% cheaper there. Raw parameters are only re-sent when they
  change (command data per frame -22%); a switch between the raw and GPU-path programs sends only the 9 registers that
  differ (`port/src/gfx3ds/c3d_fast.c`: citro3d's output for both configurations is captured once and diffed; 18
  words instead of ~100, `fastswitch=0` to disable); vertices are written as whole blocks; the near/depth test of each
  load is 12 products instead of 8 corner transforms; the minimap flushes only its own columns. Open: skipping the
  two shader constants on a switch (both programs now carry identical ones) drew one Jabu-Jabu surface too bright
  in one of seven Azahar runs, never reproduced - they are still re-sent (12 words).
- **Next for the raw path:** lighting per limb splits draws (each limb's model-space light directions are uniforms);
  view-space lighting with a per-slot normal matrix would let limbs share a draw again, as the CPU path does.
- **Hardware measurement in v53:** `raw_vtx_ab=1` alternates the raw path every 2 reports; with `perf_ab=1` the Old 3DS
  phases also turn the render thread off (an Old 3DS has no core 2). Each report adds a memory probe (`perf mem probe
  ...`: ticks per 32-byte line for word stores, block stores, loads and cache flush, linear memory vs heap) and finer
  profiler stages (`vtx.box`, `vtx.raw`, `raw emit`, `c3d context`, `c3d uniforms`, `c3d draw`).
- **60 fps in 3D, honestly:** New 3DS - 2D is at 60 most of the time; in 3D the GPU (17-20 ms per update for the
  three frames) and the render thread (16-21 ms walk + replays) both approach the 50 ms update in big views (Kokiri
  Forest overview); v53 lowers both (less CPU, 3.5x less vertex data for the GPU to fetch). Old 3DS - no: one 268 MHz
  core has to run logic + audio (~13 ms per update) and the drawing; 20 frames per second (the N64's rate) needs the
  drawing at ~37 ms per frame, about half of v49's. v53's cuts are expected to bring it to roughly 13-15 drawn frames
  per second at full game speed; the rest needs static room geometry converted once and re-used across frames.

### 2026-10-04 (later): v54 on hardware, system-core share and IPC (v55)

- **v54 (raw path, mixer share 80%):** the Old 3DS mixer kept up (5-6.7 ms per task instead of 12-18 at 30%), but the
  game's audio engine took 3-4x longer than in v49 on both speeds (New 3DS ~4 ms per update, Old 3DS speed 10-14 ms),
  in-between frames 2-3x longer, although Azahar shows no change. The audio engine asks the DSP service (on the system
  core) to flush each buffer; with 80% of that core reserved for the game, the services there answer late - and the
  share had been reserved on the New 3DS too (for perf_ab's second worker). Within the session the raw path was
  faster on both consoles (frames shown, raw on vs off: New 3DS +10-20%, Old 3DS speed +15-25%).
- **v55:** no system-core share on a New 3DS; 55% on an Old 3DS until `audio_share_ab` measures 30/55/80; cache flushes
  through the kernel (`Port3ds_CacheFlush`, svcFlushProcessDataCache) instead of the GSP/DSP services; raw triangles
  that join loads with different light sets (limb seams: Link's hat, hardware photo) or raw and processed loads are
  lit on the CPU per vertex - also -12% draws and -11% replay time in Azahar.

### 2026-10-05: in-between frames by copy, the walk draws the first one (1.0)

- **Observation:** an update's in-between frames (t = 1/3, 2/3) and its logic frame differ only in the palette
  matrices (each slot's rows for that t) and the skinned-vertex blend (stp.y = 1 - t): the same draws, states,
  textures and vertices (the GPU path never rewrites the vertex buffer). Replaying the draw log through citro3d for
  each of them rebuilt the same command words three times - in Kokiri Forest 4.8 ms per in-between frame in Azahar,
  about a third of a walk.
- **Copy and patch (`replay_copy`, default on):** the walk now draws the update's first shown frame itself (with that
  frame's matrices) and its command words are captured: `c3d_fast.c C3Df_CaptureBegin` marks every piece of citro3d
  state for re-sending (the list citro3d re-sends after the HOME menu, minus the shader code) so the words do not
  depend on what was on the GPU before, and `C3D_UpdateUniforms` reports where each uniform run lands. Every other
  frame of the update is a `memcpy` of those words into the command buffer with the palette rows and stp.y patched
  for its t. The draw log is still recorded: it is replayed when a copy cannot be used (the 3D slider moved between
  frames, a full command buffer) and when the walk has to stop recording (an off-screen render) - then the frame
  already drawn is patched in place to the logic frame's matrices.
- **Exactness:** `replay_copy_check=1` replays the draw log for every copied frame instead and compares its command
  words with the patched copy: 0 differing words over ~10,000 frames (Kokiri Forest, Link's house, the attract demo's
  scenes, 3D toggling every 2 s, render thread and CPU/GPU overlap off); the one exception is a boot frame that
  contains citro3d's one-time capture of the two vertex-program configurations. Getting there fixed an old replay
  inaccuracy: the first replay used to start from the draw state the walk ENDED in (gfx_pc.c only sends what changed
  since the previous frame), so its first draws could get the walk's last alpha test or texture; the log now starts
  with the walk's initial state, and every frame starts from the frame's own viewport and fixed values of the
  per-frame uniforms.
- **Cost (Azahar):** in-between frame 4.83 -> 0.77 ms (Kokiri Forest), 2.47 -> 0.62 ms (attract demo; walk + first
  frame 18.5 -> 17.2 ms): about a fifth less render CPU per update on the New 3DS. At Old 3DS speed (Azahar CPU clock
  25%, the Old 3DS thread layout): Link's house 28.0 -> 39.9 (copies) -> 45.9 (walk draws the first frame) frames
  shown per second; updates with both in-between frames 0 -> 133 of 300.
- **Old 3DS raw-path rules (`raw_relax=1`, an option, off by default):** a load whose box reaches the camera stayed on
  the CPU path entirely; now it stays raw and only triangles with a vertex at or behind the eye go to the CPU clipper
  (the raw shader already clamps depth at the near plane like the NoN microcode), and deep loads are not split for
  screen-linear shading - the Old 3DS already uses coarse splits. Kokiri Forest: raw triangles 72% -> 85%, drawing
  -8%; but the tour's framebuffer error against the N64 over 20 scenes rose from 5.8 to 6.9 (Royal Family's Tomb
  1.3% -> 4.3% of pixels off), so it is not the default.
- **Old 3DS, honestly:** heavy scenes stay far from 60. Kokiri Forest at Old 3DS speed draws one frame in about
  71 ms (logic ~10 ms per update on top): every other update is skipped, ~10 frames shown per second. What is left
  is spread over the whole display-list interpreter (8,900 commands, ~400 texture loads and ~350 draws per frame in
  Kokiri Forest), with no single hot spot; halving it needs the static geometry of rooms (and actors' static
  display lists) converted once and re-used across frames - the next step, since 60 fps on the Old 3DS is a 1.0
  goal. Interiors and smaller scenes get in-between frames now.

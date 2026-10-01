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
- `port/include/port_interp.h`: G_NOOP tags (`w0 = G_NOOP | 0x6E << 16 | op << 8 | flags`, `w1 = id`),
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

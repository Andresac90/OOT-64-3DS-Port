# Renderer overview

How the port turns the game's N64 display lists into 3DS frames. This page is the current design; the measurements
and history behind each decision are in [3ds-60fps-plan.md](3ds-60fps-plan.md) (dated lab notebook),
[3ds-stereo-3d.md](3ds-stereo-3d.md) and [PORT_ROADMAP.md](../PORT_ROADMAP.md).

## Files

| File | Job |
|---|---|
| `port/src/gfx/gfx_pc.c` | F3DEX2/RDP display-list interpreter (started from the Super Mario 64 PC port's Fast3D): matrices, vertex loads, triangles, texture cache and import, combiner keys, 60 fps interpolation, stereo offsets |
| `port/src/gfx3ds/gfx_citro3d.c` | citro3d backend: TEV combiner programs, textures, draw submission, the GPU/raw vertex uniforms, frame recording and replay-by-copy |
| `port/src/gfx3ds/c3d_fast.c` | cheap vertex-program switch and the command-word capture used by replay-by-copy |
| `port/src/gfx3ds/gfx_3ds.c` | frame begin/end, render targets (2D / stereo / widescreen), flip presenter, stereo convergence, depth copies for the game |
| `port/src/gfx3ds/shader*.v.pica` | vertex programs: `shader.v.pica` (CPU path), `shader_gpu.v.pica` (GPU path), `shader_raw.v.pica` (raw path) |
| `port/src/3ds_main.c` | frame pacing, frame skip, render thread, settings, performance report |

## One update, three frames

The game logic runs at the N64's 20 updates per second (`R_UPDATE_RATE` 3); the screen refreshes 60 times. Each
update builds one display list, which is drawn once by walking it, and twice more as in-between frames:

1. **Walk** (`gfx_run`): the interpreter translates every command. Draws go to citro3d and are recorded.
2. **Interpolation** (`fps60=1`): game-side tags (`port/include/port_interp.h`, emitted as `G_NOOP` groups by
   actors, skeleton limbs, skin, the view and the sky) identify each matrix across updates; the in-between frames
   blend each tagged transform between the previous and current update at t = 1/3 and 2/3. No guessing from
   display-list position: untagged matrices are drawn as-is, teleports and camera cuts are not blended.
3. **Replay by copy** (`replay_copy=1`): the walk's citro3d command words are captured once; each in-between frame is
   a copy with only the matrix palette rows and one interpolation uniform patched (proven word-exact against a full
   replay, `replay_copy_check=1`). Falls back to replaying the recorded draw log on mode switches or overflow.

The flip presenter (`gfx_3ds.c`) shows each finished frame on its own refresh, so the game thread never waits for
vertical blank.

## Three vertex paths

The N64 RSP transformed, lit, fogged and clipped vertices; the port splits that work by what the PICA200 can do
exactly. Chosen per vertex load (`gfx_sp_vertex_gpu`) and per triangle:

| Path | Who transforms | Used for |
|---|---|---|
| **Raw** (`raw_vtx=1`) | `shader_raw.v.pica` on the game's own 16-byte `Vtx`, copied as-is: matrix from a palette slot, up to 4 directional lights, texture scale, fog, stereo | most geometry |
| **GPU** (`gpu_vtx=1`) | `shader_gpu.v.pica` on vertices the CPU packed (model space + slot) | loads the raw path can't take: texgen, more than 4 lights, skinned 60 fps loads, 2D |
| **CPU** | the interpreter (N64 math), `shader.v.pica` only places the result | triangles crossing the near plane or the guard band (N64 clipping), and the N64's screen-linear shading where the GPU's perspective-correct interpolation would differ visibly |

Rules that keep the GPU paths N64-exact (measured with `tools/statediff/fbdiff.py` against the N64 in ares):
- a load leaves the raw path for the GPU path if any corner of its bounding box is behind or near the eye, or its far
  side is more than `raw_ratio` / 10 (default 3) times deeper than its near side (screen-linear shading would show);
- triangles split for screen-linear shading decide **per edge**, from the edge's own data, so neighbours agree and
  no T-junction cracks appear on hardware (`tjdump=<frame>` + `tools/tjunctions.py` measure them; Azahar can't
  show them);
- off-screen rejection uses the load's bounding box in all three frames' cameras, so objects at the edge of a fast
  camera turn don't vanish from in-between frames.

The RDP combiner becomes a TEV program per combiner key (`gfx_citro3d.c compileTev`), cached in a pool.

## Stereo 3D

A per-vertex horizontal shift in clip space, `x += sep * (w - convergence)` (true off-axis stereo, slope 1 on both
sides of the screen plane; any nonlinear per-vertex curve cracks shared edges). Convergence follows Link, limited so
the nearest on-screen surface pops out at most 25% (`STEREO_POPOUT_MAX`) and nothing breaks the screen borders.
The sky is drawn at infinity, the HUD and pre-rendered rooms at fixed depths (tagged by the game). Details:
[3ds-stereo-3d.md](3ds-stereo-3d.md).

## Threads and consoles

- **New 3DS:** game logic on core 0, drawing on core 2 (`render_thread=1`), audio mixer on core 2. 60 frames per
  second in 2D and 3D.
- **Old 3DS:** one core for game and drawing, audio mixer on the system core (55% share). Frame skip and the speed
  rules (`speed_rules`: all triangles on the GPU's fast path, no screen-linear splits) keep the game at full speed;
  frames shown depend on the scene. A New 3DS can simulate it: `o3ds_sim=1` + `o3ds_layout=1`.

## Measuring

- `prof=1`: sampling profiler by stage plus a performance report every 300 updates in `sdmc:/3ds/oot/boot.log`.
- `*_ab=1` settings alternate one option every few reports, for A/B on hardware within one session.
- `tools/regress.sh` (boot gate), `tools/statediff/` (N64 vs 3DS state and framebuffer comparison over every scene),
  `tools/pcprof.py` (sampling profiler through Azahar's GDB stub). Azahar timings are instruction counts, not hardware
  time: cache misses, threading and GPU precision must be checked on a console.

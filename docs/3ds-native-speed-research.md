# Native speed on 3DS: research and plan (2026-10-01)

Goal: the game at the N64's full speed (20 updates per second) on both consoles, and 60 frames shown
per second where the hardware allows it. This page collects what other N64-to-3DS ports did, with
their measured results, and ranks what applies here.

## Where the time goes today

Hardware profile, Old 3DS speed (New 3DS at 268 MHz without L2, `perf_ab=1`), GPU vertex path, one
update takes about 98 ms against the N64's 50 ms:

| Stage | Share | Notes |
|---|---|---|
| Triangle emit | 15% | packing 56-byte vertices per triangle |
| Triangle split | 14% | CPU fallback that reproduces the N64's screen-linear shading |
| Game logic | 14% | the decompiled game itself |
| Vertex + lighting | 11% | lighting still on the CPU |
| Triangle setup | 9% | |
| Matrices | 7% | |
| Display-list walk | 7% | |
| Audio (main thread) | 6% | the mixer itself already runs on core 1 |
| Submit / flush | 10% | |

Drawing is about 80% of an update; the game logic is small. On a New 3DS the same frame is about three
times faster, enough for full speed and 45 to 54 frames shown per second.

## What other ports did

| Project | Hardware result | Main techniques |
|---|---|---|
| [Wyatt-James/sm64-3ds-port](https://github.com/Wyatt-James/sm64-3ds-port) (Super Mario 64), branch `cleanup-and-optimization` | Near 60 fps on Old 3DS, reported by [Super Mario 64 3DS Port Ultimate](https://github.com/Epic0522/Super-Mario-64-3ds-port---Ultimate) | The whole N64 vertex stage runs in the PICA200 vertex shader ("Emu64"): G_VTX only stores pointers to the raw 16-byte N64 vertices, triangles copy them into the vertex buffer, and the shader reads 16-bit positions and texture coordinates and 8-bit normals and colors, then transforms, lights, generates environment-map coordinates and scales textures. Measured steps in its history: GPU texture coordinates -1.4 ms, GPU color combiner 11.54 to 11.05 ms, vertex-buffer creation 5.6 to 4.8 ms, no function-pointer render API -0.8 ms, custom citro3d/libctru builds -1.6 ms, render target not re-selected per draw (20-30% fewer GPU commands), one shader binary with shared uniforms, VRAM textures, native texture formats. Audio thread on core 1 (Old 3DS) / core 2 (New 3DS), mixer in assembly. |
| [mkst/sm64-port](https://github.com/mkst/sm64-port/tree/3ds-port), [RetroGamer02/sm64](https://github.com/RetroGamer02/sm64) | Full speed on Old 3DS (community reports) | Audio thread on core 1 / core 2, "naive frame skip if a frame takes longer than 33.3 ms", `-O3 -ffast-math` on the renderer |
| [cruxxxxxx/gdx-3ds](https://github.com/cruxxxxxx/gdx-3ds) (F-Zero X, built with Claude) | 59.6 fps median on New 3DS, Old 3DS unsupported | Render thread on the New 3DS's spare core 2, one frame ahead of the game: hardware median 49 to 56 fps (pipe mode), 59.6 with "ahead" mode. Per-frame memo caches in the display-list bridge (-42% of that stage), HUD texture atlas, automatic level of detail. GPU vertex transform was measured as imperceptible on hardware there (their vertex stage was about 1 ms). Notes that Azahar time-slices the cores on one host thread, so threading gains can only be measured on hardware. [Post-mortem](https://cruxxxxxx.github.io/gdx-3ds/) |
| [999sian/soh-3ds](https://github.com/999sian/soh-3ds) (Ship of Harkinian) | New 3DS target; Old 3DS "native 20 FPS presentation" in Azahar, gameplay on a real Old 3DS not yet verified | CPU vertex transform with a passthrough shader, audio worker on core 1/2, assets converted on the console from the user's ROM, distributes a CIA without game data. Research branch on running the audio mixer on the DSP: a single DSP operation measured slower than the ARM11 on an Old 3DS. |
| [TiBlin/2ship2harkinian-3DS](https://github.com/TiBlin/2ship2harkinian-3DS) (Majora's Mask) | New 3DS only, "60 FPS is a target, not a guarantee" | Interpolation with adaptive skipping of in-between frames, PICA200 backend |

System facts ([3dbrew: Multi-threading](https://www.3dbrew.org/wiki/Multi-threading)): applications get
core 0 fully; core 1 (Old and New 3DS) only for the share set with `APT_SetAppCpuTimeLimit`; core 2 on
the New 3DS is available to applications; core 3 is reserved. Scheduling is priority-based, without
time slices between threads of equal priority. This port already runs the audio mixer on core 1 (Old
3DS, 55% share) and core 2 (New 3DS).

## Plan, ranked by expected gain per risk

1. **Frame skip on Old 3DS (done, emulator-verified).** When the game is more than half an update
   behind, an update is not drawn; logic and audio still run. Never two in a row, never a frame the
   game reads back. Azahar with its CPU at 25% (half speed, like the Old 3DS): 10.25 to 19.5 updates
   per second, 11.4 frames shown per second. Automatic on Old 3DS and in `perf_ab`'s Old 3DS phase;
   `frameskip=0/1` in `settings.txt` overrides. Needs a hardware check.
2. **Render thread on New 3DS core 2.** The game already double-buffers its display lists, as on the
   N64 where the RSP and RDP render one frame while the CPU builds the next. Rendering frame N on core 2
   while core 0 runs update N+1 overlaps the two; F-Zero X gained about 10 fps median from it. Shared
   state to protect: segment table, texture cache invalidations, the depth and color readbacks, the
   pause-menu capture. The audio worker also lives on core 2 and keeps a higher priority. Cannot be
   measured in Azahar; hardware A/B behind a setting.
3. **Full N64 vertex stage on the GPU (the SM64 Emu64 design).** Raw 16-byte vertices, lighting,
   environment-map coordinates and fog in the shader. Removes most of the vertex, emit and split stages
   (about 40% of an Old 3DS update). Trade-off: the screen-linear shading split is dropped where it
   applies (Old 3DS, or a setting), and lighting runs in floating point instead of the RSP's fixed
   point. Every step measured with `fbdiff` against the N64.
4. **Smaller measured items from SM64:** avoid re-selecting the render target per draw, shader
   programs in one binary with shared uniforms, native texture formats instead of RGBA8 for every
   texture, textures in VRAM, a lighter matrix path.
5. **Save writes:** each SRAM write rewrites the 32 KB `save.bin` with `"wb"`; SM64 measured stdio
   writes stalling for seconds. Time a save on hardware.

## Tried: screen-linear shading on the GPU (2026-10-01)

The N64 interpolates shade and fog linearly on the screen and only textures with perspective. Instead of
splitting triangles on the CPU, the GPU shader divided positions by w (all attributes screen-linear) and kept
texture unit 0 perspective-correct with projective texturing. In Azahar it matched the CPU split exactly
(101-scene average error 5.92 for both) with fewer CPU triangles. On a New 3DS (v29) textures smeared and
streaked, most likely from the limited precision of the real GPU's texture-coordinate interpolation with
projective coordinates. It is off by default (`shade_linear=1` enables it). Lesson: Azahar cannot validate
precision-sensitive GPU techniques; they need a hardware test first.

Also measured: turning the split off entirely (`shade_split=0`, how PC ports draw) saves about 7% of the work
but raises the average error from 5.92 to 6.24, with visibly flat, washed-out floors and walls (Castle
Courtyard 8.9 to 30.8).

## Done: split thresholds and direct drawing on Old 3DS (2026-10-02)

Sweep of the N64-shading split thresholds (GPU path, 7 most shading-sensitive scenes + benchmark):

| Ratio / min px / depth | Split share | Triangles per frame | Accuracy |
|---|---|---|---|
| 1.15 / 10 / 6 (before) | 6.5% | 5285 | reference |
| 1.3 / 16 / 4 | 2.9% | 4294 | Jabu-Jabu +0.3 |
| 1.5 / 24 / 3 | 1.6% | 3969 | Jabu-Jabu +0.5, full tour 5.917 -> 5.986 |
| 2.0 / 32 / 2 | 0.6% | 3778 | every scene worse (Jabu-Jabu +2.8) |

1.5 / 24 / 3 is used only while frame skip is on (Old 3DS). With it, and with frames drawn directly when no
in-between frame fits (always on Old 3DS), Azahar at Old 3DS speed went from 91.5 to 79.5 ms of drawing per
frame.

## Measurement rules

- Azahar measures CPU work (instruction count); hardware is 1.5 to 3 times slower and decides.
- Threading gains are invisible or negative in Azahar; they are measured only on hardware.
- Every accuracy-relevant change is checked with the 101-scene tour and `fbdiff` (today: average
  error 5.40, 96 of 101 scenes with identical game state).

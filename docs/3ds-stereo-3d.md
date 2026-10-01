# 3DS stereoscopic 3D

The 3D slider works. When the slider is down there is no extra cost: the renderer uses the normal mono
mode (800px wide with anti-aliasing on hardware).

## Design

- **Mode switch** (`port/src/gfx3ds/gfx_3ds.c`, `gfx_3ds_update_stereo`): this runs every frame
  *before* `C3D_FrameBegin`, because citro3d `svcBreak`s on `C3D_RenderTargetDelete` inside a frame.
  - Slider > 0.02: the mono target is deleted and a 240×800 target is created, holding both 400×240
    eyes side by side along the long axis. The top screen switches to `gfxSetWide(false)` +
    `gfxSet3D(true)`, and the mode becomes `GFX_3DS_MODE_STEREO`.
  - Slider back to 0: the mono target is recreated and the old mode restored.
  - The two targets never exist at the same time; the 480×800 AA target alone takes 3 MB of VRAM.
- **Output:** two output-only targets, `sEyeOut[0/1]`, are created with no depth. Their colour buffers
  are freed and pointed at the two halves of the stereo target (the tiled buffer is contiguous along
  the long axis). They are linked to `GFX_TOP` `GFX_LEFT` and `GFX_RIGHT`. Before `C3D_FrameEnd` they
  are marked `used`, so citro3d's own FrameEnd transfers and swaps them. There is no CPU stall.
- **Drawing** (`gfx_citro3d.c`, `draw_triangles`): each batch is drawn twice from the same vertex
  buffer. Per eye, the viewport and scissor are re-issued with a +400 long-axis offset and the `eye`
  uniform is set. The display list is processed once. There is no render-target switching and no
  replay. Off-screen targets (pause-menu Link, etc.) are drawn once, with no shift.
- **Shader** (`shader.v.pica`): positions arrive in clip space, transformed on the CPU. The shift is
  `clip.y += eye.x * max(0, w - eye.y)`. The clip y axis is the screen's horizontal axis, because the
  target is portrait.
  - In NDC this is `eye.x * (1 - conv / w)`, so anything at `w <= conv` stays at screen depth. That
    covers the HUD, text, and 2D/ortho draws (w = 1). The world recedes into the screen, and nothing
    pops out.
  - Constants: `STEREO_CONVERGENCE 80`. Per-eye shift = slider × 0.03 NDC, which is about 12 px total
    disparity at infinity at full slider. The left eye gets −shift; the sign was measured.
- **Depth modes** (`gfx_citro3d_set_stereo_mode`):
  - 0 = by distance (default).
  - 1 = infinity (conv 0, full shift). Used by the skybox: OoT's sky is a small box around the camera,
    so its own w would put it at screen depth, in front of the terrain.
  - 2 = fixed depth just behind the characters (0.9 × shift; 0.6 put the picture in front of them). Used by pre-rendered rooms (shop/house skyboxes, S2DEX
    backgrounds), so 3D actors aren't pushed behind a wall that appears in front of them.
  - `src/code/z_vr_box_draw.c` sets the mode with `gDPNoOpTag(0x3D5E3D0m)` (the N64 ignores G_NOOP).
    `gfx_pc.c` handles the tag in the G_NOOP case and sets mode 2 around `gfx_s2dex_bg_rect`.
- **Readbacks:** depth (lens flare / light glows), PreRender capture (pause background) and the
  statediff color readback use one eye (the left half, laid out exactly like normal mode) via
  `ViewW/ViewH`. Their buffers are dropped on a switch and reallocated. The capture buffer is sized for
  the mono target, which is the larger of the two.

## Verifying in Azahar

1. Set `factor_3d=100` and `render_3d=1` (side by side) in
   `~/Library/Application Support/Azahar/config/qt-config.ini`. Azahar reports the factor as the
   slider position.
2. Create `sdmc:/3ds/oot/capture_stereo`. Every 300 stereo frames the full 240×800 target (8-byte
   header w, h, then detiled RGBA8) is written to `stereo_fb.bin`.
3. Measure the per-region horizontal shift between the eyes. Expected at full slider: sky and horizon
   +12 px (right eye image to the right = behind the screen), close-up Link and the HUD 0.
4. Restore the config and remove the flag afterwards.

## Known limits / next steps

- Hardware performance is not measured yet. Draw calls double; the pixel count is half that of the
  mono 800px-AA mode.
- Switching from stereo back to mono (slider down) has only been exercised on hardware, not in the
  emulator.
- Convergence and strength are constants; a settings option could expose them.

## v2 (2026-09-30, after hardware feedback "not deep enough; Link pops out in 2D rooms")

- **Eye sign (confirmed on hardware):** the PICA framebuffer is stored bottom-up, so viewport
  long-axis offset 0 lands in the *second* half of the buffer in memory, which `sEyeOut[1]` sends to
  GFX_RIGHT. So e = 0 draws the **right** eye and gets **−shift**. In `stereo_fb.bin` dumps (memory
  order) the first half is the **left** eye. "Behind the screen" means the right-eye image sits right
  of the left-eye image, which is a positive shift in `stereo2png`'s "eye1 = eye0 shifted by".
  (v11 flipped the sign after misreading the dump. Everything popped out and it hurt; restored in v12.)
- **Convergence follows Link:** `gPortStereoFocusW` = the player's projectedW (z_play.c), smoothed in
  `gfx_3ds_update_stereo`, clamped to 60..600. Link sits at the screen plane and the world recedes.
  Closer things come forward, at most 10% of the full depth (`STEREO_POPOUT_LIMIT`; v11 had 35%).
- **Shader:** `out.y += (eye.x · clamp(1 − conv/w, popLimit, 1) + eye.w) · w`, with no shift when w < 1.5
  (2D/ortho). eye.w is a constant shift for flat layers.
- **Depth modes** (G_NOOP tags `0x3D5E3D0m`):

  | Mode | Use | Shift |
  |---|---|---|
  | 0 | 3D geometry | by distance |
  | 1 | sky | full, constant |
  | 2 | pre-rendered room picture | 0.4 × shift, just behind Link at the screen |
  | 3 | the whole HUD (`Interface_Draw` tags itself) | flat at screen depth |

- **Strength:** 0.04 NDC per eye at full slider, about 16 px at infinity. Measured in Hyrule Field:
  see the v12 measurements below.
- **PICA quirk:** viewport origins are **signed 10-bit**. A right-eye viewport starting at ≥ 512 on the
  long axis (the A button's, at 524) wrapped negative and vanished from that eye (the left one). Such viewports
  start at 400, are made taller, and clip y is remapped in the shader (`remap` uniform:
  `y' = a·y + b·w`, with `a = h/(dy+h)` and `b = dy/(dy+h)`).

## v3 (2026-09-30, after hardware feedback: white crack dots on the ground in 3D, near floor flat)

- **Cause:** a per-vertex *curve* in the shader (the pop-out clamp). The renderer splits big triangles
  and clips them, and new vertices on an edge are only on the neighbour's edge if the shift is affine
  in the clip-space position. The curve bent those edges (T-junction cracks showing the white clear),
  and the clamp pinned everything nearer than the limit to one flat depth.
- **Now:** the depth offset is a vertex attribute (`v4`, `PVtx.s`) computed on the CPU:
  `s = w − c` behind the convergence (true stereo) and `s = 0.2 · (w − c)` in front of it (gentle).
  Triangles crossing the plane w = c are split on it (`gfx_emit_tri`). Inside every piece s is exactly
  affine, and split/clip interpolation carries it like x/y, so there are no cracks and no chord error.
  (A smooth curve interpolated across a ground triangle spanning w = 38..1000 took the far vertex's
  depth: the near floor measured *behind* Link.)
- **Shader:** `out.y += eye.x · s + eye.w · w`. Ortho/2D (w exactly 1) has s = 0.
- **Measured, Kokiri Forest:** walls +11 px (behind), Link 0, floor around and below him ≈ 0 with a gentle
  forward gradient, HUD 0.

## v4 (2026-09-30): research-based design — the current one

### What the research says

| Principle | Source | How the port applies it |
|---|---|---|
| Use parallel-axis **asymmetric-frustum (off-axis)** stereo, not toe-in: it has no vertical parallax or keystone, and gives a controllable zero-parallax plane. | Paul Bourke; the citro3d `Mtx_PerspStereoTilt` (±iod/2 shift plus a projection skew at the `screen` distance) | The clip-space shift `x' = x ± e·(w − c)` is exactly the off-axis projection: horizontal only, zero at w = c. |
| **Stereo window violations** are a primary cause of discomfort: something in front of the screen plane cut off by the frame border. | MSU stereo-quality reports; Solid Sight "The Stereo Window"; floating-window papers | Automatic convergence: 13 probes along the bottom and side borders find the nearest 3D surface there, and the screen plane goes no further than it. Border geometry is never in front. |
| **Zone of comfort** is about ±0.5 diopters around the screen. It narrows when depth changes quickly. | Shibata, Kim, Hoffman & Banks, JOV 2011; the rate-of-change VAC study (Vision Research 2014) | Max separation 16 px at infinity (≈ 3.4 mm, ≈ 0.65° at 30 cm on a New 3DS: inside the 1° safety guideline and far inside Shibata's zone). The convergence recedes slowly and approaches fast. |
| Keep crossed parallax (pop-out) small; guidelines give ≈ 2–3% of screen width at most. | 3D Consortium safety guidelines; the "percentage rule" literature | Pop-out only happens for things nearer than both Link and the border geometry, and not touching an edge. |
| HUD/UI at the screen plane. OoT3D itself adjusted HUD-like elements (the Z-target mark) for depth. | Iwata Asks: OoT3D development staff | The HUD and title logo are flat layers (mode 3). |

### Design

- **Depth per vertex** (v3): `s = w − c` for all 3D geometry (pure off-axis, `STEREO_NEAR_SLOPE = 1`),
  carried as a vertex attribute. It's exactly affine, so there are no cracks when triangles split.
- **Convergence** `c = min(Link's w, border depth)`, clamped to 10..600:
  - Border depth is the 3rd-nearest of 13 border probes (`gfx_pc.c stereo_probe_tri`: homogeneous
    barycentrics, exact even with vertices behind the camera).
  - It follows asymmetrically (0.3 toward nearer, 0.04 toward further).
  - In pre-rendered rooms, the target is halved.
- **Flat layers:** the HUD (`Interface_Draw`) and the title logo (`EnMag_Draw`).

### Measured (Azahar, full slider)

| Scene | Bottom ground | Link | Far | HUD |
|---|---|---|---|---|
| Title intro over Hyrule Field (v13: bottom −20 px, popped out) | +4…+7 | — | sky +15…+16 | — |
| Hyrule Field | 0 | +4 | +13 | 0 |
| Kokiri Forest | 0 | +7 | +13 | 0 |

Positive values are behind the screen.

### Further options (not done)

- A floating window (a black mask on one eye's edge) for objects that do cross the side borders in front.
- Expose strength and convergence as settings.

### Menus (v15)

The file select and the pause menu draw their panels as perspective 3D. Under automatic convergence
they came out at mixed depths ("hurts the eye" on hardware). These gamestates set
`gPortStereoFlatScene` each frame (`FileSelect_Main`; `Play_Draw` while `IS_PAUSED`). 3D geometry is
then a flat screen-depth layer, and the sky / room background sits at half depth behind it. Measured:
file-select panels and text at 0, sky behind; all pause pages at 0.

### Pre-rendered room edges (v18)

The room picture is shifted sideways for depth, which uncovered its edge: a "void" strip beside shop
rooms in 3D (hardware v17). In pre-rendered rooms, everything but the HUD is now zoomed horizontally by
the shift plus 50% (`sRoomZoom`, shader `eye.z`). Zooming the picture *and* the 3D actors keeps them
aligned; it acts as a slight FOV change. Measured in the Kokiri Shop: no void pixels at the 4:3 edges
in either eye.

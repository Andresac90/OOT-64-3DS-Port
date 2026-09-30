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
  - 2 = fixed middle depth (0.6 × shift). Used by pre-rendered rooms (shop/house skyboxes, S2DEX
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

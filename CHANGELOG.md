# Changelog

No game builds are published; each version is a source release. See the [README](README.md) to build.

## Unreleased (toward 1.0)

The title ID changed to `0xF0C64`: uninstall the previous version with FBI once. Saves are not affected.

Plan and criteria: [docs/RELEASE_1.0.md](docs/RELEASE_1.0.md).

### Added
- Frame skip on Old 3DS: when the game falls behind the N64's schedule, an update is not drawn (never
  two in a row), so the game keeps its full speed. `frameskip=0/1` in `settings.txt` overrides.
- HOME Menu icon and banner (original artwork), version in `boot.log`.
- `tools/make_link_banner.py`: optional HOME Menu banner (and icon with `--icon`) with Link's 3D model, rendered locally from your own game data (`port/banner_local.bnr`, `port/icon_local.png`, never committed).
- HOME Menu icon: a glowing fairy (original artwork, tools/make_icon.py); `tools/make_navi_icon.py` renders Navi
  from your own game data instead (`port/icon_local.png`, never committed).
- The GPU vertex path is the default (`gpu_vtx=0` returns to the CPU one): as accurate, faster on both consoles.
- `aa=0/1` in `settings.txt`: anti-aliasing on/off (measurement: `aa_ab=1` alternates it).
- Frame interpolation toward 60 fps: the game keeps the N64's 20 updates per second, and the renderer
  draws in-between frames from transforms the game tags (models, skeletons, skinned meshes, camera, sky).
- GPU vertex path (`gpu_vtx=1`): vertices are transformed and fogged by the 3DS GPU. It is as accurate
  as the CPU path and faster on New 3DS.
- Measurement switches in `settings.txt`: `prof=1` (CPU profile), `perf_ab=1` (New 3DS alternates
  full speed and Old 3DS speed), `gpu_ab=1` (alternates the vertex paths).
- Stereoscopic 3D: convergence on Link, menus and the HUD at screen depth.
- Touch screen: top-screen HUD on/off, Navi on the VIEW button.
- New 3DS C-Stick turns the camera (`cstick=0` in `settings.txt` makes it the four C buttons again).
- New 3DS ZR presses the BOOTS pad (cycles the owned boots).

### Changed
- Anti-aliasing is off by default (`aa=1` turns it on): it more than doubled the GPU's work per frame; New 3DS in 2D
  went from about 38 to 53 frames shown per second without it (the 800-pixel-wide mode stays).
- The CPU builds the next frame while the GPU draws the previous one (`overlap=0` to disable): the three frames of
  an update used to take turns with the GPU, which cost the most in 3D.
- 60 fps pacing: frames are drawn into a ring of buffers and a small thread shows each at its refresh (flip
  presenter, `flip=0` to disable): the game never waits for the screen, frames are never lost or torn, and a late
  frame repeats the previous one for a refresh instead of slowing the game.

- Touch screen: the third item pad is labelled "I" as in *Ocarina of Time 3D* on every model (D-pad down and, on New
  3DS, ZL press it too; a small "ZL"/"ZR" tag shows on New 3DS only). ZR no longer doubles C-up (VIEW and D-pad up).
- Log lines are written to the SD card by a background thread: the measurement report (`prof=1`) used to pause the
  game for up to half a second each time it was written.

### Fixed
- Widescreen pause menu: black bars at the top left and right (the background's first strip stayed 4:3 because the
  previous draw went to Link's off-screen preview).
- 60 fps: objects at the screen edge popped in late while the camera turned fast; the renderer now keeps everything
  visible in any of the in-between frames, not only in the update's own frame.
- Touch screen: item and boots icons are centered on their art (some sit up to 2 pixels off-center in their
  texture), the tab labels on their pixels, and the "not available" grey is readable.
- Widescreen: pre-rendered rooms stay 4:3 for a second after their picture (camera switches flashed 3D geometry in
  the side bars); the pause menu's background keeps the whole width; actors near the edges no longer pop while the
  camera turns fast (drawn a margin past the edge while in-between frames trail the camera).
- Touch screen: the "TOP HUD" label did not fit its button (now "HUD"); labels are centered on their pixels.
- Thin bright cracks along some triangle edges on hardware (title screen ground, more visible in 3D): the
  N64-shading split and the near-plane/guard-band clipping added vertices on edges that the neighbouring
  triangle did not have (T-junctions). Edges are now split by a rule that depends on the edge alone, so both
  triangles always agree (`tools/tjunctions.py` counts them per frame; title ground: 67 to 0 on screen), at the
  same cost and accuracy.
- File select: the name entry keyboard, file names and the death counter were blank. The US version loads
  these glyphs from the Japanese kanji font through `Kanji_OffsetFromShiftJIS`, which the decompilation has only
  as MIPS assembly; the port's stub returned the space glyph for every character. Now implemented in C
  (`port/src/kanread_port.c`, table generated from `src/code/kanread.s`); name entry matches the N64.
- Saves: a power loss or a removed SD card during a save could destroy `save.bin`. The new save is written
  completely to `save.tmp` first, the previous one is kept as `save.bak`, and the file is written once per save
  (0.5 s after the game finishes saving, or at once when the software is closed) instead of once per SRAM write.
- 3D mode: actors popping at the left and right edges while the camera moved (each eye sees a little past
  the N64's frame; the culling margin now includes the 3D shift).
- Voices and drums: every voice (Navi's "Hey! Listen!", Link's yells) and every drum in the music was silent.
  An N64 address check ("is this pointer relocated?") failed for every 3DS address.
- Cutscene music: music started or stopped by cutscenes (the Great Deku Tree's talk and others) never played;
  the command's sequence number was read from the wrong byte.
- GPU vertex path: floor and wall edges showed while walking, because pieces of triangles cut on the CPU stayed
  still in the 60 fps in-between frames.
- Pause menu: playing a song from Quest Status now plays its demo, as on the N64 (it was skipped).
- Sound effects now stop at scene changes, as on the N64.
- Shiny metal (sword, shields): the game's camera direction for environment maps was ignored.
- Link not riding Epona on the title screen (the touch Ocarina button triggered every frame).
- Black side bars in widescreen in scenes without a sky; actors disappearing at the edges.
- 3D not coming back after the pause menu (video memory).
- Shading of the N64 logo and other effects (GPU color combiner clamping).

### Performance
- Old 3DS: coarser N64-shading splits while frame skip is on (split share of the frame 12.8% -> 2.2% at Old 3DS
  speed; full accuracy kept on New 3DS), and frames are drawn directly instead of being recorded for
  in-between frames that never fit: 13% less drawing time per frame in the emulator at Old 3DS speed.
- `boot.log` stays quiet during play: bring-up traces removed, the performance report only with a
  measurement switch on.
- Audio mixing on the second CPU core; ARM SIMD in the audio microcode.
- Fewer redundant GPU state changes and texture binds; cheaper shading on flat triangles.

## milestone-2026-09-30

First version that plays like a 3DS game, verified on a New 3DS and in the Azahar emulator.

- Boot through the N64 logo, title screen and file select into gameplay.
- Renderer verified against the N64 frame by frame (`tools/statediff`).
- Music and sound effects matching the N64.
- Widescreen, with 4:3 kept for pre-rendered backgrounds.
- Touch panel in the style of *Ocarina of Time 3D*, with a live minimap.
- HOME button, saves on the SD card, Old 3DS support.

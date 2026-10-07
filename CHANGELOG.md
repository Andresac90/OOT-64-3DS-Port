# Changelog

No game builds are published; each version is a source release. See the [README](README.md) to build.

## Unreleased

### Added
- Menus: the D-pad moves the cursor like the stick on the file select and the pause screens (it stays the C
  buttons in gameplay, dialogue and ocarina playing).
- Touch screen, dungeons: floor buttons left of the map, as on the pause map page. Tapping a floor shows its map
  (visited rooms, the outlines with the Map, Link's room highlighted, a yellow arrow at his floor); tapping it again
  or the map goes back to the room map.

### Fixed
- Stereoscopic 3D: objects in the middle of the picture could come far out of the screen at full 3D (the Deku Tree
  cutscene's Navi, right at the camera: uncomfortable on hardware). The nearest on-screen 3D surface now comes out at
  most a quarter of the depth at infinity (~4 px at the full slider); the screen plane moves nearer when it would
  come out further. Frame-edge geometry and Link keep their existing rules.
- The HOME Menu crashed (hardware, Luma crash dumps: a null-pointer read in the HOME Menu) when it showed the
  stereoscopic 3D banner (a CGFX model made with pycgfx). Fixes tried on hardware - index lists for every mesh, the
  banner animation `COMMON`, the SMDH "extended banner" flag - did not stop it. The banner is now a flat picture
  in bannertool's standard banner, the format shown fine on hardware before: the original artwork for everyone
  (`tools/make_banner3d.py --flat`), Link on a transparent background from your own game data (`tools/make_link_banner.py`;
  `--white`: on white). The
  stereoscopic banner stays available as an experiment (`make_banner3d.py` without `--flat`,
  `make_link_banner.py --stereo`).
- Widescreen in 2D: pre-rendered rooms (the Kokiri houses, shops) drew wide, showing their hidden geometry at the
  sides, and rooms that 3D showed in 4:3 were wide in 2D. Each frame starts with the GPU scissor off, and the
  renderer re-sent its 4:3 scissor only when it changed; it is now sent again every frame (3D was right: each eye's
  draw sets it).
- "HUD off" hid the Z-target reticle; it now stays.
- The pause screen's dungeon map showed no rooms, even with the Map. Its palette is built byte by byte in game
  memory, which the renderer reads in the layout of loaded ROM data (each 8 bytes reversed): it came out scrambled,
  mostly transparent. The page now loads a copy in that layout.
- C-stick camera: turning the view into a hillside or a wall made it shake and look doubled (Hyrule Field). The
  eye now stops at the collision point instead of fighting the game's own swing, and the stick no longer lowers the
  camera into the ground at Link's feet.
- 60 fps, C-stick camera: turning the camera made Link look doubled and the sky shake. The in-between frames blended
  the camera's view matrix part by part, and its translation (the world origin seen from the camera, thousands of
  units away) fell short of the turn's arc: the ground near Link jumped about 4 pixels back and forth on every
  in-between frame (measured in Azahar). The camera's position is now blended and the translation rebuilt from it:
  the steps are even.
- The sun showed twice (four half-suns on hardware). Its display list loads its textures as 8-bit, but they are
  4-bit: read as 8-bit, each texture row held two rows of the picture side by side. The N64 shows one round sun
  (checked in ares); the 3DS now draws it with 4-bit loads.
- 60 fps: a rupee blinking before it vanishes stood up as a huge spike in the in-between frames (the actor's shadow
  took the rupee's place in the matching on the frames it was hidden). Shadows are now matched on their own.
- Touch screen: the rupee and key counts sat on the top row of their icons; the number is now centered on the
  icon's height, and icon and number are centered in the left column like the buttons above them.

### Changed
- README: build instructions for Linux, Windows (WSL) and macOS, with the commands for devkitPro and `makerom`.
- README screenshots show an early game (3 hearts, no items, only the Kokiri Sword and the Deku Shield) instead
  of the debug save's full inventory: `tools/make_showcase.py` builds with the new `PORT_START_FRESH` option.
- No more references to another game's touch screen or to a Discord server (this project has none; the mentions
  came from the decompilation's README, kept in `docs/DECOMP_README.md`).

## 1.0.0 (2026-10-06)

Highlights of 1.0:
- The whole game: every scene of both ages loads, and the game state matches the N64's in 96-98 of 101 scenes
  (101-scene tours compared field by field; the rest are small known differences), with the original audio.
- New 3DS: 60 frames per second in 2D and in 3D (the game keeps the N64's 20 updates per second; the frames in
  between are interpolated), stereoscopic 3D, widescreen option, C-Stick camera.
- Old 3DS: the game runs at its full speed; frames are skipped when the console cannot draw them all (about 15-17
  per second in the biggest scenes such as Kokiri Forest, around 45 in interiors such as Link's house). 60 frames
  per second everywhere on the Old 3DS continues after 1.0 (docs/3ds-60fps-plan.md: the cross-frame display-list
  cache).
- A stereoscopic HOME Menu banner; built from your game data, Link stands in it as his own 3D model.
- A touch screen: C items, ocarina, boots, pause pages, minimap, HUD and screen options.
- Saves are written atomically to the SD card; the HOME menu and sleep (closing the lid) work.

The title ID changed to `0xF0C64`: uninstall the previous version with FBI once. Saves are not affected.

Plan and criteria: [docs/RELEASE_1.0.md](docs/RELEASE_1.0.md).

### Added
- Frame skip on Old 3DS: when the game falls behind the N64's schedule, an update is not drawn (never
  two in a row), so the game keeps its full speed. `frameskip=0/1` in `settings.txt` overrides.
- HOME Menu icon and banner (original artwork), version in `boot.log`.
- `tools/make_link_banner.py`: optional HOME Menu banner (and icon with `--icon`) with Link's 3D model, captured locally from your own game data (`port/banner_local*`, `port/icon_local.png`, never committed).
- `tools/make_showcase.py`: the README's screenshots, both screens, taken from the running game.
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
- In-between frames by copy (`replay_copy`, on by default): an update's 60 fps frames differ only in their matrices,
  so the walk draws the first one itself and the others are copies of its GPU commands with the matrices patched in,
  instead of each replaying the walk's draw log through citro3d. Measured in the emulator: an in-between frame
  costs a quarter to a sixth of before (attract demo 2.5 -> 0.6 ms, Kokiri Forest 4.8 -> 0.8 ms), about a fifth
  less render CPU per update; at Old 3DS speed Link's house went from 28 to 46 frames shown per second. Checked
  word for word against the draw log's replay (`replay_copy_check=1`) over thousands of frames, in 2D and 3D. The
  draw log's replay also starts from the walk's draw state now (it started from the state the walk ended in).
- `speed_rules` (alias `raw_relax`; 1 = while frame skip is on, i.e. on an Old 3DS - the default since hardware v60
  measured 15.7-17 against 12.5-13.3 frames shown in Kokiri Forest; 0 = never; 2 = always): every vertex load stays on the raw path and every triangle goes to the GPU - no N64 NoN depth
  clamp (the 3DS GPU clips at the near plane itself, as 3DS games do), no CPU clipping, no splits for the N64's
  screen-linear shading. That per-triangle CPU work was a third of Old 3DS drawing on hardware (v58). The cost: shading
  gradients like a PC port's and geometry closer to the camera than the near plane cut instead of flattened. A first
  version kept the depth clamp: near-camera ground flickered and showed cracks (hardware v59). `speed_rules_ab=1`
  alternates it for a hardware measurement.
- Old 3DS audio: one more audio frame (~16 ms) is kept queued for the DSP (`audio_margin`, samples; automatic: 512
  when the mixer shares the system core). The engine kept only 4 ms beyond the current frame, enough for the N64's
  punctual audio task; a late task left the DSP empty and it played a gap (crackling). The report now counts DSP
  underruns and dropped buffers.
- Lit models on the raw vertex path are lit in camera space by the vertex shader: one light set serves every limb of
  a character (the N64 transforms the lights into each limb's space, which split draws and lit the skin seams between
  limbs on the CPU). The camera-space normal comes from the palette rows already uploaded (OoT's modelview ends in
  world space and its view matrix sits in the projection: the projection's columns give the camera axes); matrices
  with a non-uniform scale and orthographic projections keep the N64's model-space lighting. Kokiri Forest at Old 3DS
  speed: Link's drawing 7.5 -> 6.0 ms; 101-scene tour error unchanged (5.800 -> 5.801).
- Matrices: MV x P is computed only when vertices need it (a skeleton multiplies several matrices per limb first);
  the display list's next cache line is prefetched (Old 3DS: no L2 cache, 50-190 cycles per missed line).
- Anti-aliasing is off by default (`aa=1` turns it on): it more than doubled the GPU's work per frame; New 3DS in 2D
  went from about 38 to 53 frames shown per second without it (the 800-pixel-wide mode stays).
- Render thread (`render_thread`, on by default): the game computes the next update while another CPU core draws the
  current one, as the N64's CPU and RCP did. New 3DS: drawing on core 2. Not on the Old 3DS: its second core is
  shared with the system, which keeps most of it (measured: drawing there was 3-4 times slower, and the game loop
  there could not keep up), so the Old 3DS draws on core 0 and only the audio mixer uses core 1. If the renderer ever
  stops for 2 seconds, `boot.log` names where.
- About a fifth less CPU per drawn frame (measured with the new `tools/pcprof.py`, a sampling profiler through
  Azahar's GDB stub): dead timing code from the PC port, faster palette loads and per-command bookkeeping, fewer
  per-triangle and per-vertex checks, and faster copies of two citro3d routines (`port/src/gfx3ds/c3d_fast.c`).
- Old 3DS audio: the title now allows up to 89% of the system core (`port/oot.rsf` MaxCpu; it was the template's
  30%, which silently refused the port's request). At 30% each mixer task took 12-18 ms instead of ~4, and the game
  waited on it: very low frame rate and crackling audio at Old 3DS speed (hardware v52). At 80% (v54) the mixer kept
  up, but every request to the system services on that core got slower, and the game's audio engine took 3-4 times
  longer, on a New 3DS too. Now: no share at all on a New 3DS (the mixer is on core 2), 55% on an Old 3DS
  (`audio_share`; `audio_share_ab=1` cycles 30/55/80 for a measurement). The share and the New 3DS speed mode are
  re-applied after the HOME menu or sleep, which reset them.
- CPU cache flushes for the GPU and the DSP (vertex buffers, audio buffers, the frame) go to the kernel directly
  instead of through the GPU and DSP system services.
- Visible facets on Link's hat (hardware v54): a triangle joining two limbs was lit with the first limb's light
  directions on the raw vertex path. Such triangles, and triangles joining a raw and a CPU-prepared load (which were
  dropped or got wrong colours), are now lit on the CPU with each vertex's own lights. Also fewer draws (-12%).
- Holding L while the game starts uses `settings_b.txt` instead of `settings.txt` (when present), and the previous
  session's log is kept as `boot_prev.log`: two test setups in one sitting.
- Each save writes one line to `boot.log` with its duration (or the step that failed).
- The HOME Menu banner is stereoscopic: layers at different depths for the HOME Menu's camera (the sky behind the
  screen, the title at it, the ocarina in front - or, with the optional local tool, Link's own 3D model captured from
  the game: `tools/make_link_banner.py`).
  `tools/make_banner3d.py` builds it with pycgfx (glTF to CGFX) and bannertool.
- The touch screen's NAVI button pulses (blue and green) while Navi wants to talk, so her call is noticed with the
  top-screen HUD off.
- The CIA carries the title version 1.0.0, so FBI installs it as an update and the HOME Menu refreshes its icon and
  banner.
- Touch pads no longer miss quick taps when the frame rate is low: taps between two updates are kept until the next
  one reads them.
- `render_thread=0` was written to `settings.txt` after a session in the Old 3DS layout, turning the render thread
  off for later sessions.
- Raw vertex path (`raw_vtx`, on by default; `raw_vtx_ab=1` alternates it for measurements): the game's own 16-byte
  N64 vertices go to the GPU unchanged, and the vertex shader does what the N64's RSP did per vertex (matrix,
  directional lights, texture coordinates, fog), as the Super Mario 64 3DS port does. The CPU no longer transforms,
  lights or packs most vertices, and writes 16 bytes per vertex instead of 56. Geometry near the camera or very
  deep (floors under Link, long corridors) keeps the CPU path, which reproduces the N64's screen-linear shading:
  the N64 comparison is unchanged (7 most shading-sensitive scenes). About 15% less renderer CPU in Azahar; more is
  expected on hardware, where the Old 3DS spent 12-17% of its time writing vertices.
- Smaller GPU command lists: raw-path parameters are only re-sent when they change (about a fifth less command data
  per frame), switching between the raw and the CPU-prepared vertex programs sends only the 9 GPU registers that
  differ instead of citro3d's whole program setup (18 command words instead of ~100; `fastswitch=0` restores
  citro3d's way), and vertices are written to the vertex buffer as whole blocks.
- The CPU builds the next frame while the GPU draws the previous one (`overlap=0` to disable): the three frames of
  an update used to take turns with the GPU, which cost the most in 3D.
- 60 fps pacing: frames are drawn into a ring of buffers and a small thread shows each at its refresh (flip
  presenter, `flip=0` to disable): the game never waits for the screen, frames are never lost or torn, and a late
  frame repeats the previous one for a refresh instead of slowing the game.

- Touch screen: the third item pad is labelled "I" on every model (D-pad down and, on New
  3DS, ZL press it too; a small "ZL"/"ZR" tag shows on New 3DS only). ZR no longer doubles C-up (VIEW and D-pad up).
- Log lines are written to the SD card by a background thread: the measurement report (`prof=1`) used to pause the
  game for up to half a second each time it was written.

### Changed (names)
- The app is "The Legend of Zelda: Ocarina of Time N64 3DS Port" (HOME Menu, banner caption, README).

### Fixed
- Metallic and crystal surfaces with linear environment maps (`G_TEXTURE_GEN_LINEAR`): the map was squeezed 1.57x (the
  PC-port formula `acos(-x) / 4` spans more than the plain mapping's range; GlideN64's spans the same). The blue
  warp's crystal in the Chamber of the Sages came out streaked white, hid Link and flickered as it turned: 10-11.5% of
  the pixels off against the N64 before, 3-4% after (`tools/statediff/scenarios/chamber_child.txt`).
- Color combines of the form `(A - B) * C + A` are exact now (computed as `2 * (A - lerp(A, B, C) / 2)`): they were too
  bright wherever A < B * C (the warp crystal's `(TEXEL0 - PRIM) * PRIM_LOD_FRAC + TEXEL0`).
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
- Touch panel on the bottom screen, with a live minimap.
- HOME button, saves on the SD card, Old 3DS support.

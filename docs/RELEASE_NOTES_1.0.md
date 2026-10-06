# The Legend of Zelda: Ocarina of Time N64 3DS Port, version 1.0.0

A native port of *The Legend of Zelda: Ocarina of Time* (N64, NTSC-U 1.0) to the Nintendo 3DS, built from the
[zeldaret decompilation](https://github.com/zeldaret/oot). It is not an emulator: the game's own code runs on the
3DS's ARM processor, and its N64 display lists are drawn with the 3DS GPU.

**No game data is included.** This release is source code only. You build the port from your own cartridge's ROM
(see [Building](../README.md#building)); never share the files the build produces.

## What works

- The whole game: every scene of both ages loads, and the game state matches the N64's in 96-98 of 101 scenes
  (compared field by field with the N64 running the same inputs; the rest are small known differences), with the
  original music and sound.
- New 3DS: 60 frames per second in 2D and in stereoscopic 3D. The game keeps the N64's 20 updates per second; the
  frames in between are interpolated from the game's own transforms.
- Old 3DS: the game runs at its full speed; frames are skipped when the console cannot draw them all, and smaller
  scenes and interiors get in-between frames too (in-between frames are now copies of the first frame's GPU commands
  with only the matrices changed, a fraction of their former cost).
- Stereoscopic 3D with the 3D slider, a widescreen option, the C-Stick as camera (New 3DS).
- A touch screen: C items, ocarina, boots, pause pages, minimap, HUD and screen
  options; the NAVI button pulses when Navi wants to talk.
- Saves on the SD card, written atomically; the HOME menu and sleep work; a HOME Menu banner (with
  Link when built from your game data: `tools/make_link_banner.py`).

## Known limitations

- Old 3DS: the game keeps its full speed, but the frames shown per second depend on the scene: about 15-17 in the
  biggest ones (Kokiri Forest), around 45 in interiors such as Link's house, fewer in 3D. Measured on a New 3DS
  running at Old 3DS speed and in the emulator; no Old 3DS was available. 60 everywhere on the Old 3DS is the next
  goal.
- A complete playthrough on hardware is still in progress; please report anything that differs from the N64.
- See [docs/RELEASE_1.0.md](RELEASE_1.0.md) for what was verified and how.

## Requirements

A 3DS family console with Luma3DS and a CIA installer (FBI), the DSP firmware dump (`sdmc:/3ds/dspfirm.cdc`), your
own NTSC-U 1.0 ROM, and devkitPro plus `makerom` to build. Full steps in the [README](../README.md).

Changes since the first milestone: [CHANGELOG.md](../CHANGELOG.md).

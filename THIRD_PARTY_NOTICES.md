# Third-party notices

This port builds on the work of other projects. This file lists what is used, how, and under which
terms. It covers source code only; this repository contains no Nintendo data (see the
[README](README.md#legal)).

## Code this repository contains or is derived from

| Project | Used for | License |
|---|---|---|
| [zeldaret/oot](https://github.com/zeldaret/oot) | The Ocarina of Time decompilation: all game code under `src/`, `include/`, `assets/` (descriptions only), the asset extraction tools and the N64 build. This repository is a fork of it. | No license file in the upstream repository |
| [sm64-port](https://github.com/sm64-port/sm64-port) | Fast3D renderer that `port/src/gfx/` started from | No license file in the upstream repository |
| [mkst/sm64-port, 3ds-port branch](https://github.com/mkst/sm64-port/tree/3ds-port) | citro3d backend that `port/src/gfx3ds/` started from | No license file in the upstream repository |
| [libultraship](https://github.com/Kenix3/libultraship) | Parts of the display-list interpreter in `port/src/gfx/gfx_pc.c` follow or adapt its interpreter | MIT (notice below) |
| [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) | Reference for the audio microcode math in `port/src/audio_microcode.c` (itself derived from the sm64-port mixer) | No license file in the upstream repository |
| [stb_image](https://github.com/nothings/stb) | `port/src/third_party/stb_image.h` | Public domain or MIT, at your choice (license text at the end of the file) |
| [citro3d](https://github.com/devkitPro/citro3d) 1.7.1 | `port/src/gfx3ds/c3d_fast.c`: altered copies of its `uniforms.c` and `drawElements.c` (faster uniform upload and draw call), marked as altered, with its license notice, in the file | zlib |

## Design references (no code copied)

| Project | What informed this port | License |
|---|---|---|
| [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) | The idea of tagging transforms with stable ids for frame interpolation | GPL-3.0 |
| [RT64](https://github.com/rt64/rt64) | Matrix groups and decomposed matrix interpolation | MIT |
| [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) | Labelled frame-interpolation recording, the 90-degree snap rule | See above |
| [Wyatt-James/sm64-3ds-port](https://github.com/Wyatt-James/sm64-3ds-port) | Its "Emu64" design: the game's own N64 vertices sent to the GPU and processed by the vertex shader (`port/src/gfx3ds/shader_raw.v.pica`); its use of the system core for audio on the Old 3DS | No license file in the upstream repository |
| [gdx-3ds](https://github.com/cruxxxxxx/gdx-3ds) | Hardware findings: a render thread on the New 3DS's core 2, re-applying the CPU settings after the HOME menu | See its repository |

## Libraries used at build time

Banner tools (only to regenerate `port/banner.bnr`, not part of the build): [pycgfx](https://github.com/skyfloogle/pycgfx)
converts the banner's glTF scene to CGFX (not included in this repository: no license file upstream; clone it into
`tools/pycgfx/`) and [bannertool](https://github.com/diasurgical/bannertool) packs the banner (MIT).

[devkitPro](https://devkitpro.org/) toolchain with libctru and citro3d (zlib license). They are linked into
a build that each user makes locally; no builds are distributed.

## libultraship license

```
MIT License

Copyright (c) 2022 kenix3 kenixwhisperwind@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

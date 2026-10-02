# Native Ocarina of Time (N64) 3DS Port

<p align="center">
  <img src="docs/images/showcase.png" width="460"
       alt="Kokiri Forest on the top screen; the bottom screen shows the touch panel with the live minimap, hearts, item buttons, Ocarina and Boots">
</p>

An unofficial, fan-made **native** Nintendo 3DS port of The Legend of Zelda: Ocarina of Time (N64), built on the
[zeldaret/oot](https://github.com/zeldaret/oot) decompilation. The game's own code runs natively on the 3DS
CPU; a new renderer, audio backend and dual-screen interface replace the N64 hardware.

> **This repository contains no ROM and no game assets.** To build and play, you need your own legally
> obtained copy of the N64 game (NTSC-U 1.0). No prebuilt game binaries are distributed; see [Legal](#legal).

This project is not affiliated with, endorsed by, or sponsored by Nintendo.

## Features

- **Both consoles:** New 3DS and Old 3DS (Old 3DS currently runs below full speed, see [Status](#status)).
- **Top screen:** 400×240 gameplay, original 4:3 or widescreen (toggle on the touch screen, saved).
- **Stereoscopic 3D** with the 3D slider: automatic convergence on Link, HUD and menus at screen depth.
- **Bottom screen, in the style of *Ocarina of Time 3D*:** live minimap with Link's position and chest
  markers, hearts and magic, rupees and keys, touch buttons for the C items (with ammo), first person / Navi,
  a dedicated **Ocarina** button, a **Boots** button that cycles the boots you own, and shortcuts to the
  Gear / Map / Items pause pages.
- **Optional top-screen HUD** (on by default, can be hidden from the touch screen).
- **Frame interpolation toward 60 fps** (experimental): the game logic keeps the N64's 20 updates per
  second, and extra in-between frames are shown when there is CPU time for them.
- **Accuracy work:** the renderer is compared frame by frame against the N64 running the same inputs
  (tools in `tools/statediff`), and the audio is compared against the N64's output.
- Saves to the SD card (`sdmc:/3ds/oot/save.bin`).

## Status

Work in progress. The title screen, file select and gameplay run on New 3DS at the N64's full game speed,
but not every area and dungeon has been tested yet, and known issues remain (tracked in
[PORT_ROADMAP.md](PORT_ROADMAP.md) and the issue tracker). Keep backups of `sdmc:/3ds/oot/save.bin`.

| | New 3DS | Old 3DS |
|---|---|---|
| Game speed (N64 = 20 updates/s) | full speed | about half speed |
| Frames shown per second (60 fps interpolation) | about 45–54 depending on the scene | 10 |
| Stereoscopic 3D | yes | yes |

Performance is the current focus; progress and measurements are in
[docs/3ds-60fps-plan.md](docs/3ds-60fps-plan.md).

## Requirements

- A Nintendo 3DS family console with custom firmware ([Luma3DS](https://github.com/LumaTeam/Luma3DS)) and
  [FBI](https://github.com/lifehackerhansol/FBI) (or another CIA installer).
- Your own **N64 Ocarina of Time NTSC-U 1.0** ROM in big-endian `.z64` format
  (MD5 `5bd1fe107bf8106b2ab6650abecd54d6`).
- The 3DS DSP firmware dump for sound: `sdmc:/3ds/dspfirm.cdc`. Luma3DS creates it from your own console:
  Rosalina menu (L + D-pad down + SELECT) → Miscellaneous options → **Dump DSP firmware**.
- A computer to build on (macOS or Linux; WSL on Windows) with [devkitPro](https://devkitpro.org/wiki/Getting_Started)
  (`3ds-dev`: devkitARM, libctru, citro3d, picasso, makerom).

## Building

```bash
# 1. devkitPro
sudo dkp-pacman -S 3ds-dev
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM

# 2. Your ROM (never commit it; *.z64 is ignored by git)
cp /path/to/your/oot-us-1.0.z64 baseroms/ntsc-1.0/baserom.z64

# 3. Extract the assets from your ROM (the decompilation's setup step; needs Python 3)
make setup VERSION=ntsc-1.0          # macOS: use gmake (Homebrew make) for steps 3 and 4

# 4. Generate the sources the 3DS build includes (textures, soundfonts, text)
make VERSION=ntsc-1.0 -j8

# 5. Build the 3DS port
make -f Makefile.3ds cia             # -> build/3ds/oot.cia (install with FBI)
make -f Makefile.3ds cci             # -> build/3ds/oot.3ds (for the Azahar emulator)
```

Step 3 also creates `baseroms/ntsc-1.0/baserom-decompressed.z64`, which the game reads at run time.

**Optional, HOME Menu banner with Link:** `python3 tools/make_link_banner.py` (needs Azahar, Pillow and
[bannertool](https://github.com/diasurgical/bannertool)) renders Link's 3D model from your game data into
`port/banner_local.bnr` (`--icon` also makes a Link icon, `port/icon_local.png`), which later builds use. These
files are ignored by git and must never be shared; without them the build uses the original artwork.

**Troubleshooting (macOS):** if Anaconda or Miniconda is on your `PATH`, the audio tools can link against
its libxml2 and then fail with `Library not loaded: @rpath/libxml2.2.dylib`. Run steps 3 and 4 with conda
removed from `PATH` (or after `conda deactivate`), then rebuild `tools/`.

Detailed notes, emulator testing and debugging tools: [docs/BUILDING_3DS.md](docs/BUILDING_3DS.md).

## Installation

1. Install `build/3ds/oot.cia` with FBI.
2. Copy `baseroms/ntsc-1.0/baserom-decompressed.z64` to the SD card as:
   ```
   sdmc:/3ds/oot/baserom-decompressed.z64
   ```
3. Make sure `sdmc:/3ds/dspfirm.cdc` exists (see Requirements).
4. Launch *Ocarina of Time* from the HOME Menu.

## Controls

| 3DS | Game | Notes |
|---|---|---|
| Circle Pad | Control Stick | |
| A / B | A / B | |
| Y / X | C-Left / C-Right | item buttons |
| L | Z (target) | |
| R | R (shield) | |
| START | START (pause) | |
| SELECT | L (minimap on/off) | |
| D-Pad | C buttons | the N64 D-pad is unused by the game |
| ZL / ZR (New 3DS) | C-Down / C-Up | third item / first person and Navi |
| C-Stick (New 3DS) | C buttons | |
| Touch screen | VIEW, items, Ocarina, Boots, pause pages, 4:3 / wide, top HUD | |

Every action is reachable on an Old 3DS without ZL/ZR or the C-Stick. Details:
[docs/3ds-touch-panel.md](docs/3ds-touch-panel.md).

## Settings

Most options are on the touch screen and are saved to `sdmc:/3ds/oot/settings.txt`. A few more can be
set by editing that file (one `name=value` per line):

| Setting | Default | Effect |
|---|---|---|
| `widescreen` | 0 | 1 = widescreen, 0 = original 4:3 |
| `hud` | 1 | top-screen HUD on/off |
| `fps60` | 1 | frame interpolation: 0 = off (20 fps, like the N64) |
| `frameskip` | automatic | 1 = skip drawing an update when the game falls behind (default on Old 3DS), 0 = never |
| `aa` | 1 | anti-aliasing (smoother edges, more GPU work); 0 = off |
| `gpu_vtx` | 0 | 1 = vertex processing on the GPU (faster on New 3DS; may become the default) |
| `prof` | 0 | 1 = per-stage CPU profile in `boot.log` (for bug reports about speed) |

## Bug reports

Please open an issue with the console model (Old / New 3DS), what happened and where in the game, and attach
`sdmc:/3ds/oot/boot.log` from the session (it is rewritten each launch, so copy it right after the problem).
A photo of the screen helps for graphics issues.

## Legal

- This repository contains source code, build scripts and tools only. It does **not** contain a ROM,
  extracted game assets, or any other Nintendo data. Nintendo, Nintendo 3DS, *The Legend of Zelda* and
  *Ocarina of Time* are trademarks of Nintendo; all game content belongs to Nintendo.
- The game data is extracted from **your own ROM on your own computer** at build time, and the build
  embeds it into the binary. For that reason **no `.cia`, `.3dsx` or `.3ds` builds are published** here or in
  releases: each user builds their own from their own copy of the game. Please do not upload built binaries.
- You must own the game: dump the ROM from your own cartridge. Do not ask for or share ROMs in this
  repository's issues.
- This is a non-commercial fan project, made for preservation and to play a game you own on hardware you
  own. It is provided as is, without warranty of any kind.

## Credits

- [zeldaret/oot](https://github.com/zeldaret/oot): the Ocarina of Time decompilation this port is built on.
  Its original README is kept in [docs/DECOMP_README.md](docs/DECOMP_README.md).
- The [Super Mario 64 PC port](https://github.com/sm64-port/sm64-port) (Fast3D renderer) and the
  [Super Mario 64 3DS port](https://github.com/mkst/sm64-port/tree/3ds-port) (citro3d backend), which the
  renderer started from.
- [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) and
  [libultraship](https://github.com/Kenix3/libultraship): reference for the audio mixer math and renderer
  details. Ship of Harkinian's frame interpolation design also informed this port's.
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and [RT64](https://github.com/rt64/rt64):
  reference for the transform tagging used by the 60 fps interpolation.
- [devkitPro](https://devkitpro.org/), libctru and citro3d; [stb_image](https://github.com/nothings/stb);
  the [Azahar](https://github.com/azahar-emu/azahar) emulator and the [ares](https://ares-emu.net/) N64
  emulator, used for testing.
- Built with the help of Claude (Anthropic).

Licenses and terms of the code this port builds on: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Documentation

- [docs/BUILDING_3DS.md](docs/BUILDING_3DS.md): build, emulator testing and debugging
- [docs/3ds-touch-panel.md](docs/3ds-touch-panel.md): controls, touch panel, minimap
- [docs/3ds-stereo-3d.md](docs/3ds-stereo-3d.md): stereoscopic 3D design
- [docs/3ds-60fps-plan.md](docs/3ds-60fps-plan.md): performance and 60 fps work
- [PORT_ROADMAP.md](PORT_ROADMAP.md): port status and history

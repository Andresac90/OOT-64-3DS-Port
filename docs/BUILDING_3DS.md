# Building and testing the 3DS port

This page covers the build in detail, testing in the Azahar emulator, and the measurement tools. The
short version is in the [README](../README.md#building). Commands are for macOS; on Linux use `make`
instead of `gmake`.

| File | Purpose |
|---|---|
| `Makefile.3ds` | devkitARM build: `build/3ds/oot.{elf,3dsx,3ds,cia}` |
| `port/oot.rsf` | `makerom` settings for the CIA |
| `port/` | the 3DS platform layer: renderer, audio, input, touch panel, OS shims |
| `tools/regress.sh`, `tools/emu.sh` | boot tests in Azahar |
| `tools/perfbench.sh` | repeatable CPU-cost benchmark in Azahar |
| `tools/statediff/` | frame-by-frame comparison against the N64 (ares emulator) |

## 1. One-time setup

### devkitPro
Install devkitPro's pacman ([instructions](https://devkitpro.org/wiki/Getting_Started)), then the 3DS
toolchain (devkitARM, libctru, citro3d, picasso, 3dsxtool). `makerom` (for the `.3ds` and `.cia` files) is not part
of devkitPro: download it from [Project_CTR's releases](https://github.com/3DSGuy/Project_CTR/releases) (v0.18.4
runs on every supported system; v0.19.0's Linux build needs glibc 2.38) and put it on your `PATH`. The
README's [Building](../README.md#building) section has the commands for Linux, Windows (WSL) and macOS.
```bash
sudo dkp-pacman -S 3ds-dev
# in ~/.zprofile:
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH=$PATH:$DEVKITPRO/tools/bin
```

### Tools for the decompilation's asset extraction (Homebrew)
```bash
brew install coreutils make gsed libxml2 python@3.12
```
The extraction tools need Python 3.10 or newer; macOS's own `python3` is 3.9 and fails during setup
(`TypeError: unsupported operand type(s) for |`). Create the Python environment with a newer one before step 2:
```bash
python3.12 -m venv .venv
```
If Anaconda or Miniconda is on your `PATH`, the decompilation's audio tools can link against conda's
libxml2 and fail at run time (`Library not loaded: @rpath/libxml2.2.dylib`). Run the steps below with
conda removed from `PATH` (or after `conda deactivate`).

### Your ROM
Dump your own cartridge and place the US 1.0 ROM (big-endian `.z64`, MD5
`5bd1fe107bf8106b2ab6650abecd54d6`) at `baseroms/ntsc-1.0/baserom.z64`. ROM files are ignored by git;
never commit them.

## 2. Extract the assets
```bash
gmake setup VERSION=ntsc-1.0
```
This creates `baseroms/ntsc-1.0/baserom-decompressed.z64`, the extracted assets in
`extracted/ntsc-1.0/`, and a Python environment in `.venv/`. If you only have the decompressed ROM, put it
at `baseroms/ntsc-1.0/baserom-decompressed.z64`: setup accepts it when its checksum matches.

## 3. Generate the decompilation's sources
The 3DS build includes sources that the decompilation's own build generates (textures, soundfonts, text
tables). Run its build once:
```bash
gmake VERSION=ntsc-1.0 COMPARE=0 -j8
```
This also produces an N64 ROM in `build/ntsc-1.0/`, which the 3DS port does not use. It differs from the retail ROM
(the port changes some shared sources), so the decompilation's final checksum check is turned off with `COMPARE=0`;
without it the step ends with `md5sum: WARNING: 1 computed checksum did NOT match` on Linux.

### HOME Menu art (optional)
`port/banner.bnr` is committed, so builds need nothing else. To regenerate the stereoscopic banner
(`tools/make_banner.sh`; from your game data: `tools/make_title_banner.py` for the title-screen logo and the
Hylian Shield icon, `tools/make_link_banner.py` for Link) you need bannertool, Python
3.10+ with Pillow and gltflib (`pip install gltflib pillow`), and pycgfx:
`git clone https://github.com/skyfloogle/pycgfx tools/pycgfx` (ignored by git). The tools write a side-by-side
preview of both eyes next to the banner.

## 4. Build the port
```bash
gmake -f Makefile.3ds cci    # build/3ds/oot.3ds: for Azahar
gmake -f Makefile.3ds cia    # build/3ds/oot.cia: for a console, install with FBI
```
The build embeds data extracted from your ROM, so never publish or share the resulting files.

## 5. Run in Azahar
The game reads the ROM from the SD card at run time. Copy it to Azahar's emulated SD card once:
```bash
SD="$HOME/Library/Application Support/Azahar/sdmc/3ds/oot"
mkdir -p "$SD"
cp baseroms/ntsc-1.0/baserom-decompressed.z64 "$SD/baserom-decompressed.z64"
```
Boot the `.3ds` file (the `.cia` opens an install dialog). The tools below launch Azahar themselves:

| Command | What it does |
|---|---|
| `tools/regress.sh [label]` | boots the current build, prints PASS/FAIL (hang, crash marker, or frames stopped advancing) |
| `tools/emu.sh boot [secs]` | boots and prints a summary of `boot.log` |
| `tools/perfbench.sh LABEL [secs] [settings...]` | runs the title-screen demo with the profiler and averages the CPU numbers |

Azahar's clock advances per executed instruction, so its timings are a measure of CPU work, not of real
console speed (a console is roughly 1.5 to 3 times slower). Use them to compare changes; measure real
performance on hardware with `perf_ab=1` and `prof=1` in `settings.txt`.

## 6. Logs and settings
- The game writes its log to `sdmc:/3ds/oot/boot.log` (rewritten at each launch).
- `sdmc:/3ds/oot/settings.txt` holds the options; see the README's [Settings](../README.md#settings). With
  `sdmc:/3ds/oot/settings_b.txt` present, holding L at start uses that file instead: a second setup for tests.
- Measurement settings. The performance report in `boot.log` (every 300 updates) is written only while one is on:

  | Setting | What it does |
  |---|---|
  | `prof=1` | sampling profiler by stage, memory use, the full performance report |
  | `perf_stages=1` | timer per display-list command and per audio command. Heavy: on an Old 3DS (or `o3ds_sim`) it slows the audio mixer enough to crackle, so don't use it for speed tests there |
  | `o3ds_sim=1`, `o3ds_layout=1` | on a New 3DS: Old 3DS CPU speed (268 MHz, no L2 cache) and the Old 3DS thread layout |
  | `perf_ab=1` | New 3DS alternates full speed and Old 3DS speed every 4 reports |
  | `*_ab=1` (`raw_vtx_ab`, `replay_copy_ab`, `audio_simd_ab`, `audio_margin_ab`, ...) | alternates one option every 2 reports, so one session measures both sides |
  | `fps60=0`, `gpu_vtx=0`, `raw_vtx=0` | turn off interpolation, the GPU vertex path, the raw vertex path |

  In the report, `frames displayed/s (est)` counts frames that reached the screen (a frame finished in the same
  refresh as the next one is never shown); `frames shown/s` counts frames drawn.
- Without taking the SD card out: run [ftpd](https://github.com/mtheall/ftpd) on the console and use
  `tools/net3ds.sh IP log` (fetches the logs to `build/3ds/hwlogs/`), `tools/net3ds.sh IP put` (uploads
  `build/3ds/oot.cia` to `sdmc:/cias/`, then install it with FBI) and `tools/net3ds.sh IP settings_b FILE`. Or install
  directly with FBI's Remote Install -> Receive URLs over the network and `tools/net3ds.sh IP fbi`.
- Azahar at Old 3DS speed: set `cpu_clock_percentage=25` in Azahar's `qt-config.ini` (restore 100 after). It
  approximates the Old 3DS's CPU time, not its GPU, and multi-core timing is not representative.

## 7. Comparing against the N64
`tools/statediff/` builds a reference N64 ROM from the unmodified decompilation (in a separate worktree,
`../OOT-64-3DS-Port-n64ref`), runs it in [ares](https://ares-emu.net/) and the port in Azahar with the same
inputs, and compares game state and frames. Run it with the decompilation's Python environment:
```bash
.venv/bin/python3 tools/statediff/tour.py --age child --frames 40   # every scene, both emulators
.venv/bin/python3 tools/statediff/fbdiff.py --tour tour_child_40_101 --png
```
The tour and boot-flow tools replace `build/3ds` with an instrumented build; rebuild normally afterwards.

## More
- Controls, touch panel and minimap: [3ds-touch-panel.md](3ds-touch-panel.md)
- Stereoscopic 3D: [3ds-stereo-3d.md](3ds-stereo-3d.md)
- How the renderer works: [RENDERER.md](RENDERER.md)
- Performance and 60 fps: [3ds-60fps-plan.md](3ds-60fps-plan.md)

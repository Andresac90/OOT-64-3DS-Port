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
toolchain (devkitARM, libctru, citro3d, picasso, 3dsxtool, makerom):
```bash
sudo dkp-pacman -S 3ds-dev
# in ~/.zprofile:
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH=$PATH:$DEVKITPRO/tools/bin
```

### Tools for the decompilation's asset extraction (Homebrew)
```bash
brew install coreutils make gsed libxml2
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
gmake VERSION=ntsc-1.0 -j8
```
This also produces an N64 ROM in `build/ntsc-1.0/`, which the 3DS port does not use.

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
- `sdmc:/3ds/oot/settings.txt` holds the options; see the README's [Settings](../README.md#settings).
  Measurement switches (the periodic performance report in `boot.log` is written only while one is on):
  `prof=1` (sampling profiler, memory use), `perf_ab=1` (New 3DS alternates full speed and Old 3DS speed every
  minute), `gpu_ab=1` (alternates the CPU and GPU vertex paths), `aa_ab=1` (alternates anti-aliasing),
  `cmdflush_ab=1` (alternates the frame-end cache flush), `present_ab=1` (alternates the present gate every 4 reports),
  `fps60=0` (interpolation off), `gpu_vtx=1` (GPU vertex path), `present_gate=0` (frames submitted as soon as
  drawn, as before 2026-10-02), `split_ratio` (x100), `split_px`, `split_depth` (fixed N64-shading split
  thresholds), `split_auto=0` (New 3DS: never switch to the coarse thresholds; by default they are used while
  frame skip is on and while the logic frame is about to miss its refresh).
  The report's `frames displayed/s (est)` counts frames that reached the screen (a frame finished in the same
  refresh as the next one is never shown); `frames shown/s` counts frames drawn.
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
- Performance and 60 fps: [3ds-60fps-plan.md](3ds-60fps-plan.md)

# Building & testing the 3DS port on macOS

Reconstructed 2026-09-16 after the original WSL build scripts were lost. Pieces:

| File | Purpose |
|---|---|
| `Makefile.3ds` | devkitARM build → `build/3ds/oot.{elf,3dsx,3ds,cia}` |
| `port/oot.rsf` | `makerom` spec (New-3DS: 124 MB / 804 MHz / L2) |
| `tools/emu-test.sh` | Azahar test loop (boot → screenshot → boot.log summary) |

> **Status:** `Makefile.3ds` is a first-draft reconstruction. The compile/link/shader
> machinery is standard; the **`GAME_EXCLUDE` source list** and **`ASSET_BUILDDIR`** are
> expected to need tuning at the first build (the linker names any duplicate/missing symbol).

---

## 1. One-time setup

### a. devkitPro (cross-compiler + libs)
Install the pacman package manager, then the 3DS toolchain (devkitARM, libctru, citro3d,
picasso, 3dsxtool, makerom):
```bash
open https://github.com/devkitPro/pacman/releases/latest   # install devkitpro-pacman-installer.pkg
sudo dkp-pacman -S 3ds-dev
# add to ~/.zprofile:
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH=$PATH:$DEVKITPRO/tools/bin
```

### b. Azahar emulator
```bash
open https://github.com/azahar-emu/azahar/releases/latest   # install the macOS .dmg
```

### c. Asset-extraction prereqs (Homebrew)
```bash
brew install coreutils make gsed libxml2   # python3 already present
```

### d. The baserom
Your legal NTSC-1.0 US dump belongs at `baseroms/ntsc-1.0/baserom.z64`
(md5 `5bd1fe107bf8106b2ab6650abecd54d6`). ✅ already placed.

---

## 2. Extract assets (needed before the first build)
```bash
gmake setup VERSION=ntsc-1.0
```
This creates `baseroms/ntsc-1.0/baserom-decompressed.z64`, `extracted/ntsc-1.0/`, and the
asset `.c` sources. Point `ASSET_BUILDDIR` (in `Makefile.3ds`) at wherever the generated
asset `.c` files land, so `port/asset_srcs.mk`'s `$(ASSET_SRCS)` resolves.

---

## 3. Build
```bash
gmake -f Makefile.3ds        # -> build/3ds/oot.3dsx  (+ .elf)
gmake -f Makefile.3ds cci    # -> build/3ds/oot.3ds   (Azahar direct-boot)
gmake -f Makefile.3ds cia    # -> build/3ds/oot.cia   (hardware, install via FBI)
```
First build will surface symbol clashes between decomp files and the port shims — resolve by
editing `GAME_EXCLUDE`. Common excludes: the whole `src/libultra` except `gu/` (shimmed),
`src/boot` hardware init, `audio_stop_all_sfx.c` (→ `port/src/audio_stub.c`).

---

## 4. Run / test in Azahar
The port streams the ROM at runtime from the emulated SD. Copy the decompressed ROM there once:
```bash
SD="$HOME/Library/Application Support/Azahar/sdmc/3ds/oot"
mkdir -p "$SD"
cp baseroms/ntsc-1.0/baserom-decompressed.z64 "$SD/baserom-decompressed.z64"
```
Then the autonomous loop (boots the `.3ds`, waits, screenshots, prints the boot.log summary):
```bash
tools/emu-test.sh -s 30 -n hyrule                 # 30s boot, shot -> tools/shots/hyrule.png
tools/emu-test.sh -s 20 -n pause -p "START"       # tap START mid-run
```
Boot the **`.3ds`**, never the `.cia` (the CIA prompts an install dialog). The real New 3DS
stays the final validator for color/perf/audio.

---

## 5. Notes
- Boot bypass is active (boots straight into Hyrule Field) until the title/file-select UI (D7)
  is fixed — see `PORT_ROADMAP.md` §5.
- Game code logs to `sdmc:/3ds/oot/boot.log` via `PortDbg`/`PortDbgX`, **not** stderr.
- Tune the `KEYMAP` in `tools/emu-test.sh` to your Azahar input profile before using `-p`.
- Controls, the OoT3D-style touch panel and the bottom-screen minimap: see
  [3ds-touch-panel.md](3ds-touch-panel.md).

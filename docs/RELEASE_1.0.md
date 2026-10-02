# Release 1.0: what "complete" means

Version 1.0 is published when every item below is checked. Each item has a way to verify it; "hardware"
means a real console, the rest runs in the emulator (`tools/`). Status as of 2026-10-01.

## A. The whole game plays

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | Every scene loads as child | `tour.py --age child`: 101/101 scenes, state compared with the N64 | 96/101 identical, 5 with small differences |
| [x] | Every scene loads as adult | `tour.py --age adult` | 101/101 (2026-10-02), 98 identical, 3 known small differences; frame error 6.20 |
| [x] | Pause menu song playback (Quest Status → song → A) plays the demo like the N64 | `tour.py --scenario song_child` | matches the N64 during and after the demo |
| [x] | Sound effects stop at scene changes like the N64 | `AudioMgr_StopAllSfx` restored; child tour 101/101 | done |
| [ ] | Skull Kid's ocarina memory game and the scarecrow song recording complete | scripted scenario or hardware | untested since real audio |
| [ ] | Main quest from a new file to the end credits, every dungeon and boss | hardware playthrough (saves allowed) | partly played |

## B. Stability

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | Texture memory is freed when cache slots are reused | code (`C3D_TexDelete` on reuse) | done |
| [ ] | No unmapped memory accesses in any scene | Azahar log: `grep UnmappedAccess` = 0 after the child and adult tours | 0 in the last child tour |
| [ ] | One hour of play without a crash or freeze on each console | hardware, `boot.log` | not yet on Old 3DS |
| [x] | No memory leak across scene changes | 101-scene tour with `prof=1`: linear memory free 44.9-47.4 MB, heap flat (2026-10-02); hardware sessions now log both |
| [ ] | HOME button, sleep (closing the lid), power off from the HOME menu | hardware | HOME works; sleep untested |

## C. Accuracy

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | No bring-up hacks that change game behavior (`#ifdef __3DS__` audit) | code review of all 116 blocks | done; the rest are platform adaptations, safety guards and port features |
| [x] | Distant geometry and fog match the N64 (Hyrule Field) | `fbdiff` at noon, 4:3 (Hyrule Field, Lake Hylia, Gerudo Valley) | match; the differences are edges and a 1 px horizon line. Widescreen shows areas the N64 never draws |
| [ ] | No visible culling at the screen edges in 3D mode (each eye sees past the N64's frame) | hardware, camera moving with 3D on | reported on v30; widen the culling margin in 3D as for widescreen |
| [ ] | No stray triangles in the GPU vertex path (red/cyan polygons inside Kokiri houses, hardware v30) | hardware | v31 fixes the frozen pieces in in-between frames; to recheck |
| [ ] | Frame accuracy does not regress | `fbdiff` child tour average | 5.40 today (lower is better) |

## D. Performance

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | New 3DS: full game speed (20 updates/s, like the N64) | hardware, `prof=1` | yes |
| [ ] | New 3DS: 60 frames shown per second in most scenes | hardware, `perf_ab=1` | 45 to 54 |
| [ ] | Old 3DS: full game speed | hardware, `perf_ab=1` | frame skip: 10.25 → 19.5 updates/s in Azahar at 25% CPU; hardware check pending |
| [ ] | Old 3DS: 60 frames shown per second | hardware | stretch goal; the Old 3DS CPU is about 3 times slower |
| [ ] | Default vertex path chosen (CPU or GPU) | hardware `gpu_ab=1` + `fbdiff` | GPU path: same accuracy (5.40), faster on New 3DS in the last test |

## E. User experience

| | Item | Status |
|---|---|---|
| [x] | HOME Menu icon and banner (original artwork, no Nintendo images) | `port/icon.png`, `port/banner.bnr` (tools/make_icon.py, tools/make_banner.sh); optional local Link banner/icon, `tools/make_link_banner.py` |
| [x] | Own title ID (was `0xF8000`, the homebrew template's default) | `0xF0C64`; uninstall the old title once |
| [x] | Version in `boot.log` (first line, also on the bottom screen during boot) | `git describe`, or `port/VERSION` |
| [ ] | Saving is safe and quick | atomic (save.tmp, save.bak) and one write per save (v33, emulator-tested); time it on hardware |
| [x] | Release builds log only startup and errors (bring-up traces removed) | done; the perf report only with a measurement switch on |

## F. Documentation and legal

| | Item | Status |
|---|---|---|
| [x] | README, build guide, third-party notices | done |
| [ ] | Build tested from a fresh clone on macOS and Linux | macOS done |
| [ ] | `CHANGELOG.md` complete for 1.0 | draft started |
| [ ] | GitHub Release `v1.0.0`: notes and the source download only, no game builds | at release |

Performance research and plan: [3ds-native-speed-research.md](3ds-native-speed-research.md).

## Open decision: binaries

Today each user must build the port, because the build embeds data extracted from their ROM. That is the
main barrier for most players. Projects such as Ship of Harkinian and Zelda64Recomp publish ready-to-run
programs that contain no game data and read it from the player's own ROM when they start. Doing the same
here means loading the assets from the ROM at run time instead of compiling them into the binary. That is
a large change, so it is not part of 1.0 unless decided otherwise.

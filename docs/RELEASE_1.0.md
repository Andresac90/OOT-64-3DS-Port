# Release 1.0: what "complete" means

Version 1.0 is published when every item below is checked. Each item has a way to verify it; "hardware"
means a real console, the rest runs in the emulator (`tools/`). Status as of 2026-10-03.

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
| [x] | No unmapped memory accesses in any scene | Azahar log: `grep UnmappedAccess` = 0 after the child and adult tours | 0 in the child tour and in the adult tour (2026-10-03: all 101 scenes reached, 97 identical to the N64; the 4 others are glow flags on object edges and the Ganondorf camera raycast tie, both known) |
| [ ] | One hour of play without a crash or freeze on each console | hardware, `boot.log` | not yet on Old 3DS |
| [x] | No memory leak across scene changes | 101-scene tour with `prof=1`: linear memory free 44.9-47.4 MB, heap flat (2026-10-02); hardware sessions now log both |
| [ ] | HOME button, sleep (closing the lid), power off from the HOME menu | hardware | HOME works; sleep works (v41, 2026-10-02); power off from the HOME Menu untested |

## C. Accuracy

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | No bring-up hacks that change game behavior (`#ifdef __3DS__` audit) | code review of all 116 blocks | done; the rest are platform adaptations, safety guards and port features |
| [x] | Distant geometry and fog match the N64 (Hyrule Field) | `fbdiff` at noon, 4:3 (Hyrule Field, Lake Hylia, Gerudo Valley) | match; the differences are edges and a 1 px horizon line. Widescreen shows areas the N64 never draws |
| [ ] | No visible culling at the screen edges in 3D mode (each eye sees past the N64's frame) | hardware, camera moving with 3D on | margin widened by the stereo separation (v37); in-between frames now keep everything visible to any of their three cameras (v44, hardware v42 still showed pop-in while turning fast); confirm on hardware |
| [ ] | No stray triangles in the GPU vertex path (red/cyan polygons inside Kokiri houses, hardware v30) | hardware | split pieces in model space (v35); not reported in v39, confirm on hardware |
| [ ] | No cracks between triangles (thin bright dots on the title screen ground, hardware v39) | `tjdump` + `tools/tjunctions.py`, then hardware | crack-free splitting: title ground 67 port-made T-junctions to 0 on screen; confirm on hardware |
| [x] | Frame accuracy does not regress | `fbdiff` child tour average | GPU path 5.92 (crack-free splitting: 5.917 to 5.922; lower is better), 96/101 scenes identical state |

## D. Performance

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | New 3DS: full game speed (20 updates/s, like the N64) | hardware, `prof=1` | yes |
| [ ] | New 3DS: 60 frames shown per second in most scenes | hardware | 2D: 52.6-53.8 displayed with anti-aliasing off (v41) and the present gate; v42 54.6-56 shown. v43+ flip presenter (frames never lost, the game never waits for a refresh): Azahar 59.5 shown/s in 2D, 56 with 3D on half the time; hardware pending (v45). 3D costs a second submission of every draw; next: one command list for both eyes |
| [ ] | Old 3DS: full game speed | hardware, `perf_ab=1` | frame skip: 10.25 → 19.5 updates/s in Azahar at 25% CPU; New 3DS at Old 3DS speed (v42): 17.5-21 updates/s, 9-15 frames shown |
| [ ] | Old 3DS: 60 frames shown per second | hardware | not reachable with this renderer: at Old 3DS speed a drawn frame costs ~54 ms of CPU (v42), so even 20 frames shown needs a third less; 60 needs a renderer that reuses static geometry between frames |
| [x] | Default vertex path chosen (CPU or GPU) | hardware `gpu_ab=1` + `fbdiff` | GPU (2026-10-02): same accuracy, faster on both consoles (v27) |

## E. User experience

| | Item | Status |
|---|---|---|
| [x] | HOME Menu icon and banner (original artwork, no Nintendo images) | `port/icon.png`, `port/banner.bnr` (tools/make_icon.py, tools/make_banner.sh); optional local art from the player's game data: Navi icon (`tools/make_navi_icon.py`), Link banner (`tools/make_link_banner.py`) |
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

# Release 1.0: what "complete" means

Each item has a way to verify it; "hardware" means a real console, the rest runs in the emulator (`tools/`).
Status as of 2026-10-06.

**Decision (2026-10-05):** 1.0 is released when the build is technically ready - every item that a tool or a
short hardware session can check is done. The items that need long play on hardware (marked *after 1.0*) are
covered by playing the story after the release; problems found there go into 1.0.x updates.

## A. The whole game plays

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | Every scene loads as child | `tour.py --age child`: 101/101 scenes, state compared with the N64 | 96/101 identical, 5 with small differences |
| [x] | Every scene loads as adult | `tour.py --age adult` | 101/101 (2026-10-02), 98 identical, 3 known small differences; frame error 6.20 |
| [x] | Pause menu song playback (Quest Status → song → A) plays the demo like the N64 | `tour.py --scenario song_child` | matches the N64 during and after the demo |
| [x] | Sound effects stop at scene changes like the N64 | `AudioMgr_StopAllSfx` restored; child tour 101/101 | done |
| after 1.0 | Skull Kid's ocarina memory game and the scarecrow song recording complete | scripted scenario or hardware | *after 1.0*: hardware play (the ocarina input and the audio path it reads are exercised by the pause-menu song playback, which matches the N64) |
| after 1.0 | Main quest from a new file to the end credits, every dungeon and boss | hardware playthrough (saves allowed) | *after 1.0*: the story playthrough on hardware; every scene of both ages loads and matches the N64 in the tours |

## B. Stability

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | Texture memory is freed when cache slots are reused | code (`C3D_TexDelete` on reuse) | done |
| [x] | No unmapped memory accesses in any scene | Azahar log: `grep UnmappedAccess` = 0 after the child and adult tours | 0 in the child tour and in the adult tour (2026-10-03: all 101 scenes reached, 97 identical to the N64; the 4 others are glow flags on object edges and the Ganondorf camera raycast tie, both known) |
| after 1.0 | One hour of play without a crash or freeze on each console | hardware, `boot.log` | New 3DS: sessions of 6-8 minutes in 2D and 3D without a crash or freeze (v54-v56); Old 3DS mode 4 minutes (v57). *After 1.0*: long sessions |
| [x] | No memory leak across scene changes | 101-scene tour with `prof=1`: linear memory free 44.9-47.4 MB, heap flat (2026-10-02); hardware sessions now log both |
| after 1.0 | HOME button, sleep (closing the lid), power off from the HOME menu | hardware | HOME and sleep work (hardware); the system CPU settings are re-applied after them (v55). *After 1.0*: power off from the HOME menu |

## C. Accuracy

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | No bring-up hacks that change game behavior (`#ifdef __3DS__` audit) | code review of all 116 blocks | done; the rest are platform adaptations, safety guards and port features |
| [x] | Distant geometry and fog match the N64 (Hyrule Field) | `fbdiff` at noon, 4:3 (Hyrule Field, Lake Hylia, Gerudo Valley) | match; the differences are edges and a 1 px horizon line. Widescreen shows areas the N64 never draws |
| [x] | No visible culling at the screen edges in 3D mode (each eye sees past the N64's frame) | hardware, camera moving with 3D on | not reported since v44 (three-camera culling), many 3D sessions since. *After 1.0*: report if seen |
| [x] | No stray triangles in the GPU vertex path (red/cyan polygons inside Kokiri houses, hardware v30) | hardware | not reported since v35, many sessions since |
| [x] | No cracks between triangles (thin bright dots on the title screen ground, hardware v39) | `tjdump` + `tools/tjunctions.py`, then hardware | not reported since the crack-free splitting (v41) |
| [x] | Frame accuracy does not regress | `fbdiff` child tour average | 1.0 (2026-10-05, raw vertex path on): 5.807, the best so far (2026-09-30: 5.92; lower is better), 96/101 scenes identical state |

## D. Performance

| | Item | How it is verified | Status |
|---|---|---|---|
| [x] | New 3DS: full game speed (20 updates/s, like the N64) | hardware, `prof=1` | yes |
| [x] | New 3DS: 60 frames shown per second in most scenes | hardware | v55 (2026-10-05): 59.3-59.8 frames shown per second in every report, 2D and 3D (whole reports in 3D included); the render thread, the CPU/GPU overlap, the raw vertex path, and no system-core reservation on the New 3DS (an 80% reservation in v54 had slowed every system-service request) |
| [x] | Old 3DS: full game speed | hardware, `perf_ab=1`, then the Old 3DS layout (`settings_b.txt`, hold L at start) | Old 3DS mode on a New 3DS (268 MHz, no L2, the Old 3DS thread and audio layout), hardware v57: 20 updates/s with the mixer at 55% of the system core (30%: 13-19 updates/s and crackling; 80%: same speed, slower system services); 12-13 frames shown in Kokiri Forest (v57), 15.7-17 with the speed rules (v60), more in smaller scenes. On a real Old 3DS: *after 1.0* (no Old 3DS available) |
| after 1.0 | Old 3DS: 60 frames shown per second | hardware | **Moved after 1.0 (decision 2026-10-05: release with the New 3DS at 60 and keep working on the Old 3DS).** Since then: speed rules (hardware v60: Kokiri Forest 15.7-17 vs 12.5-13.3 frames shown), camera-space lighting (Link's drawing -20%). 2026-10-05: in-between frames are copies of the first frame's GPU commands (`replay_copy`), so interiors reach ~45 at Old 3DS speed (emulator), but big scenes stay ~10 (Kokiri Forest: ~71 ms of CPU per drawn frame against a 50 ms update). Earlier status: at Old 3DS speed a drawn frame costs 25-130 ms of CPU depending on the scene (the CPU translating N64 display lists; reading data the CPU has not touched yet costs 50-190 cycles per 32-byte line there, hardware memory probe). The game keeps its full speed and shows 15-50 frames per second in 2D; 60 needs static geometry converted once and reused between frames - the next step |
| [x] | Default vertex path chosen (CPU or GPU) | hardware `gpu_ab=1` + `fbdiff` | GPU (2026-10-02): same accuracy, faster on both consoles (v27) |

## E. User experience

| | Item | Status |
|---|---|---|
| [x] | HOME Menu icon and banner (original artwork, no Nintendo images) | stereoscopic banner (2026-10-05, `tools/make_banner3d.py`: sky behind the screen, title at it, ocarina in front; layers placed for the HOME Menu camera); icon `port/icon.png`; optional local art from the player's game data: Link in front in the 3D banner (`tools/make_link_banner.py`), Navi icon (`tools/make_navi_icon.py`). Hardware check of the 3D banner: pending |
| [x] | Own title ID (was `0xF8000`, the homebrew template's default) | `0xF0C64`; uninstall the old title once |
| [x] | Version in `boot.log` (first line, also on the bottom screen during boot) | `git describe`, or `port/VERSION` |
| [x] | Saving is safe and quick | atomic (save.tmp, save.bak), one write per save; Azahar: a new file writes 3 saves of 108-141 ms, data identical to the N64 (`bootflow.py newfile`); each save logs its duration (`[save] written, ms`) for hardware reports |
| [x] | Release builds log only startup and errors (bring-up traces removed) | done; the perf report only with a measurement switch on |

## F. Documentation and legal

| | Item | Status |
|---|---|---|
| [x] | README, build guide, third-party notices | done |
| [x] | Build tested from a fresh clone on macOS and Linux | 2026-10-05, re-run 2026-10-06 for the release tree (macOS: setup, N64 ROM, `.3ds`, `.cia`, all exit 0), exactly the files a clone gets plus the ROM, the documented steps: macOS builds and boots (Azahar boot test); Linux (Debian 12 in devkitPro's devkitARM Docker image, makerom 0.18.4) builds the same `.3ds` and `.cia`. Found and documented: the asset tools need Python 3.10+ (macOS's own is 3.9), `makerom` is not part of devkitPro (Project_CTR 0.19.0's Linux build needs glibc 2.38; 0.18.4 runs on Debian 12), and step 4 needs `COMPARE=0` (the N64 ROM it also builds differs from retail) |
| [x] | `CHANGELOG.md` complete for 1.0 | section 1.0.0 (2026-10-06) with highlights; `port/VERSION` 1.0.0 (the version of source downloads without git); the CIA's title version 1.0.0. Release text: [RELEASE_NOTES_1.0.md](RELEASE_NOTES_1.0.md) |
| [ ] | GitHub Release `v1.0.0`: notes and the source download only, no game builds | at release |

Performance research and plan: [3ds-native-speed-research.md](3ds-native-speed-research.md).

## Open decision: binaries

Today each user must build the port, because the build embeds data extracted from their ROM. That is the
main barrier for most players. Projects such as Ship of Harkinian and Zelda64Recomp publish ready-to-run
programs that contain no game data and read it from the player's own ROM when they start. Doing the same
here means loading the assets from the ROM at run time instead of compiling them into the binary. That is
a large change, so it is not part of 1.0 unless decided otherwise.

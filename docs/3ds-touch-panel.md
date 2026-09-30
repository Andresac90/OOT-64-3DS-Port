# 3DS controls, touch panel and on-screen minimap

This page describes how the 3DS port maps controls, what the bottom-screen touch panel does, and how
it is built. The design follows *Ocarina of Time 3D*. The Old 3DS layout is complete without
ZL/ZR or the C-stick. The New 3DS extras are shortcuts only.

## Button mapping (`port/src/3ds_main.c`, `Port3ds_PollInput`)

| 3DS | N64 / game action | Notes |
|---|---|---|
| Circle pad | Control stick | about ±156 → ±80 |
| A / B | A / B | |
| Y / X | C-left / C-right | item slots 1 and 3 |
| L | Z (target) | |
| R | R (shield) | |
| START | START (pause) | |
| SELECT | N64 L | shows or hides the minimap |
| D-pad up/down/left/right | C-up / C-down / C-left / C-right | the N64 D-pad is unused by OoT |
| **New 3DS** ZL | C-down | item slot 2 |
| **New 3DS** ZR | C-up | first person / Navi |
| **New 3DS** C-stick | the four C buttons | |
| Touch panel | see below | |

## Touch panel layout (320×240 bottom screen)

```
+--------+--------------------------+--------+
|  VIEW  |  hearts (10 per row)     |  Y     |  C-left item + ammo
| (C-up) |  magic bar               | ZL     |  C-down item + ammo
| rupees |                          |  X     |  C-right item + ammo
| keys   |        MINIMAP           |        |
| SCREEN |  (parchment, centred,    |        |
| 4:3/WIDE|  zoomed to the map)     |        |
|OCARINA |                          | BOOTS  |
|        | [GEAR]  [MAP]  [ITEMS]   |        |
+--------+--------------------------+--------+
```

| Pad | Behaviour |
|---|---|
| VIEW | Holds C-up (first person, or talk to Navi). |
| Y / ZL / X | Hold the matching C button. The pad shows the equipped icon (live from `interfaceCtx->iconItemSegment`), the ammo count (red at 0), and is greyed when the game disables that button. The "ZL" label only appears on a New 3DS. |
| OCARINA | Tap to play the owned ocarina without putting it on a C button. It works like a virtual fifth item button (`Port_VirtualOcarinaItem`), so the game's normal rules still apply. It is refused where the ocarina is restricted, where every C button is disabled, and in bombchu bowling. |
| BOOTS | Tap to cycle through owned boots: Kokiri → Iron → Hover (Iron and Hover need adult Link). It uses the same path as the pause menu (`Inventory_ChangeEquipment` + `Player_SetEquipmentData`) and plays the "decide" sound. |
| SCREEN | Tap to toggle 4:3 and widescreen. Saved to `sdmc:/3ds/oot/settings.txt`. |
| GEAR / MAP / ITEMS | Press START with that pause page preselected (`gPortTouchPage` → `KaleidoSetup_Update`). Tapping any tab while paused closes the menu (START). |
| rupees / keys | Display only. Keys show only in scenes where the N64 HUD shows them (`gPortHudKeys`, set in `Interface_Draw`). |
| hearts / magic | Display only: 10 hearts per row, filled by quarter; the magic bar is double length with double magic. |

**Minimap:** the minimap is on the touch screen by default (`gPortMinimapOnBottom = 1`), and the top
screen no longer draws it. SELECT (N64 L) still hides or shows it, with the game's own sound. The map
only appears where the game would draw one: overworld scenes, and dungeons once the Map item is
owned. The player/start arrows need the compass in dungeons (they are always shown on the
overworld). Chest (unopened only) and boss markers come from the map-mark data. It also follows the
game's fade: the map is blank whenever `minimapAlpha == 0` (cutscenes, some HUD modes).

## How it works

### Game side (all under `#ifdef __3DS__`; N64 builds are unchanged)

| File | Change |
|---|---|
| `port/include/port_minimap.h` | The shared interface: `PortMinimap` (map texture pointer/format/size/colour, N64-screen positions of the map, arrows, entrance icons and chest/boss marks), `PortHudInfo`, and the `gPortTouch*` request flags. |
| `src/code/z_map_exp.c` | `Minimap_ExportToBottom()` runs from `Minimap_Draw`. It fills `gPortMinimap` using the same visibility rules and positions as the drawing code, then skips the top-screen draw. The compass positions reuse the `Minimap_DrawCompassIcons` math: the overlay is an ortho view with 1 unit = 1 pixel and the origin at the screen centre, so `x = 160 + (R_COMPASS_OFFSET_X + posX/R_COMPASS_SCALE_X)/10` and `y = 120 - (R_COMPASS_OFFSET_Y - posZ/R_COMPASS_SCALE_Y)/10`. |
| `src/code/z_map_mark.c` | `MapMark_Export()` copies the chest/boss marks of the current dungeon room (at `x + 204`, `y + 140`, as `MapMark_DrawForDungeon` does). |
| `src/code/z_parameter.c` | `Interface_Draw` publishes `gPortHudIconSeg` and bumps `gPortHudSerial` (used to tell whether the HUD, and so the icon pointer, is live). It also sets `gPortHudKeys` where the small-key counter is drawn. |
| `src/overlays/actors/ovl_player_actor/z_player.c` | Adds the virtual OCARINA button (`Port_VirtualOcarinaItem`, also accepted by the "put away the item" check) and `Port_CycleBoots()`. It must **not** be index 4 of `Player_GetItemOnButton`: there, index 4 means "no item button pressed". Reusing it made Link play the ocarina every idle frame, including the title demo (fixed 2026-09-30). Both are consumed in `Player_ProcessItemButtons`, so they only act when the player could use an item anyway. |
| `src/code/z_kaleido_setup.c` | On START, `gPortTouchPage` (if ≥ 0) selects the pause page. |

### Port side

| File | Change |
|---|---|
| `port/src/ultra_shims.c` | Defines the globals, `Port_GetHudInfo()` (reads the save: rupees, keys, health, magic, boots, ocarina, C items/disabled/ammo) and `Port_GetItemIcon()` (reads any 32×32 icon from `icon_item_static` in ROM once and caches 8; used for boots and the ocarina). |
| `port/src/3ds_main.c` | Renders the panel in software straight into the bottom RGB565 framebuffer. The framebuffer is rotated: pixel (x, y) is at `fb[x*240 + (239-y)]`. It is single-buffered by `consoleInit`, and `GSPGPU_FlushDataCache` runs after drawing. Text uses the libctru console font with a drop shadow. Buttons are a bevelled stone look with per-pixel grain. Each element redraws only when its value changes. The map redraws every other frame while the game feeds it, and is cleared once it stops (more than 6 frames stale). |
| `port/src/3ds_compat.c` | Once the panel is up, `PortCompat_SilenceStderr()` routes both stdout and stderr to a discard stream, so no console text lands on the panel. `Log()` goes only to `boot.log`. |

**Byte order:** game RAM data (DMA'd textures, icons, map textures) keeps logical byte `k` at
address `k ^ 7` on the 3DS, the same rule the renderer's `gfx_src_swizzle` uses. The panel reads
icons as `rgba[o ^ 7]` and map nibbles as `tex[(i >> 1) ^ 7]`. Reading them straight gave magenta
icons and a scrambled map.

**Map formats:** dungeon maps are I4 (colour = prim `100,255,255`, alpha = intensity). Overworld maps
are IA4 (3-bit intensity × `R_MINIMAP_COLOR`, 1-bit alpha). Those colours are designed for a dark
3D scene, so the panel darkens them onto the parchment. The drawn texels are cropped to their
bounding box and scaled to fit (at most 3×), because the N64 textures place the map in a corner.

## Verifying (emulator, no screenshots)

1. `touch "~/Library/Application Support/Azahar/sdmc/3ds/oot/capture_bottom"`. With that flag
   present, the panel writes `bottom_fb.bin` every 300 polls (a 12-byte header w, h, fmt, then raw
   RGB565).
2. Get into gameplay, for example with
   `tools/statediff/tour.py --skip-n64 --age child --scenes SCENE_DEKU_TREE --frames 700`.
   It needs an `{}` placeholder at the `build/statediff/n64_tour_*.json` path it reports; delete
   it afterwards.
   The tour's HUD mode keeps `minimapAlpha` at 0, so to see the map there, temporarily bypass the
   `minimapAlpha == 0` check in `Minimap_ExportToBottom` and revert it afterwards.
3. Convert the dump to PNG. It is rotated 240×320 in the file, and pixel (x, y) is at
   `(x*240 + 239-y)*2`.
4. Remove the flag file afterwards.
5. Run `tools/regress.sh`. The tour overwrites `build/3ds`, so delete the hooked objects first.

## Not done yet / ideas

- Tabs don't switch pages while paused; they close the menu, like START.
- Map textures only; no fog-of-war rooms or floor selector as in OoT3D.
- No B-button (sword) display. The I/II item slots of OoT3D are not added; the three N64 C slots are
  kept.
- The arrow direction follows `shape.rot.y` with world +x to the right and +z down. This has been
  checked against the arrow position, but not against a side-by-side N64 capture.

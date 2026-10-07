#include "gfx.h"
#include "gfx_setupdl.h"
#include "controller.h"
#include "map.h"
#if PLATFORM_N64
#include "n64dd.h"
#endif
#include "printf.h"
#include "regs.h"
#include "segment_symbols.h"
#include "sfx.h"
#include "sys_matrix.h"
#include "terminal.h"
#include "translation.h"
#include "map_mark.h"
#include "play_state.h"
#include "player.h"
#include "save.h"

#include "assets/objects/gameplay_keep/compass_arrow.h"
#include "assets/textures/parameter_static/parameter_static.h"

MapData* gMapData;

s16 sPlayerInitialPosX = 0;
s16 sPlayerInitialPosZ = 0;
s16 sPlayerInitialDirection = 0;
s16 sEntranceIconMapIndex = 0;

void Map_SavePlayerInitialInfo(PlayState* play) {
    Player* player = GET_PLAYER(play);

    sPlayerInitialPosX = player->actor.world.pos.x;
    sPlayerInitialPosZ = player->actor.world.pos.z;
    sPlayerInitialDirection = (s16)((0x7FFF - player->actor.shape.rot.y) / 0x400);
}

void Map_SetPaletteData(PlayState* play, s16 room) {
    s32 mapIndex = gSaveContext.mapIndex;
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    s16 paletteIndex = gMapData->roomPalette[mapIndex][room];

    if (interfaceCtx->mapRoomNum == room) {
        interfaceCtx->mapPaletteIndex = paletteIndex;
    }

    PRINTF_COLOR_YELLOW();
    PRINTF(T("ＰＡＬＥＴＥセット 【 i=%x : room=%x 】Room_Inf[%d][4]=%x  ( map_palete_no = %d )\n",
             "PALETE Set 【 i=%x : room=%x 】Room_Inf[%d][4]=%x  ( map_palete_no = %d )\n"),
           paletteIndex, room, mapIndex, gSaveContext.save.info.sceneFlags[mapIndex].rooms,
           interfaceCtx->mapPaletteIndex);
    PRINTF_RST();

    interfaceCtx->mapPalette[paletteIndex * 2] = 2;
    interfaceCtx->mapPalette[paletteIndex * 2 + 1] = 0xBF;
}

void Map_SetFloorPalettesData(PlayState* play, s16 floor) {
    s32 mapIndex = gSaveContext.mapIndex;
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    s16 room;
    s16 i;

    for (i = 0; i < 16; i++) {
        interfaceCtx->mapPalette[i] = 0;
        interfaceCtx->mapPalette[i + 16] = 0;
    }

    if (CHECK_DUNGEON_ITEM(DUNGEON_MAP, mapIndex)) {
        interfaceCtx->mapPalette[30] = 0;
        interfaceCtx->mapPalette[31] = 1;
    }

    switch (play->sceneId) {
        case SCENE_DEKU_TREE:
        case SCENE_DODONGOS_CAVERN:
        case SCENE_JABU_JABU:
        case SCENE_FOREST_TEMPLE:
        case SCENE_FIRE_TEMPLE:
        case SCENE_WATER_TEMPLE:
        case SCENE_SPIRIT_TEMPLE:
        case SCENE_SHADOW_TEMPLE:
        case SCENE_BOTTOM_OF_THE_WELL:
        case SCENE_ICE_CAVERN:
        case SCENE_DEKU_TREE_BOSS:
        case SCENE_DODONGOS_CAVERN_BOSS:
        case SCENE_JABU_JABU_BOSS:
        case SCENE_FOREST_TEMPLE_BOSS:
        case SCENE_FIRE_TEMPLE_BOSS:
        case SCENE_WATER_TEMPLE_BOSS:
        case SCENE_SPIRIT_TEMPLE_BOSS:
        case SCENE_SHADOW_TEMPLE_BOSS:
            for (i = 0; i < gMapData->maxPaletteCount[mapIndex]; i++) {
                room = gMapData->paletteRoom[mapIndex][floor][i];
                if ((room != 0xFF) && (gSaveContext.save.info.sceneFlags[mapIndex].rooms & gBitFlags[room])) {
                    Map_SetPaletteData(play, room);
                }
            }
            break;
    }
}

void Map_InitData(PlayState* play, s16 room) {
    s32 mapIndex = gSaveContext.mapIndex;
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    s16 extendedMapIndex;

    switch (play->sceneId) {
        case SCENE_HYRULE_FIELD:
        case SCENE_KAKARIKO_VILLAGE:
        case SCENE_GRAVEYARD:
        case SCENE_ZORAS_RIVER:
        case SCENE_KOKIRI_FOREST:
        case SCENE_SACRED_FOREST_MEADOW:
        case SCENE_LAKE_HYLIA:
        case SCENE_ZORAS_DOMAIN:
        case SCENE_ZORAS_FOUNTAIN:
        case SCENE_GERUDO_VALLEY:
        case SCENE_LOST_WOODS:
        case SCENE_DESERT_COLOSSUS:
        case SCENE_GERUDOS_FORTRESS:
        case SCENE_HAUNTED_WASTELAND:
        case SCENE_HYRULE_CASTLE:
        case SCENE_DEATH_MOUNTAIN_TRAIL:
        case SCENE_DEATH_MOUNTAIN_CRATER:
        case SCENE_GORON_CITY:
        case SCENE_LON_LON_RANCH:
        case SCENE_OUTSIDE_GANONS_CASTLE:
            extendedMapIndex = mapIndex;
            if (play->sceneId == SCENE_GRAVEYARD) {
                if (CHECK_QUEST_ITEM(QUEST_SONG_NOCTURNE)) {
                    extendedMapIndex = 0x14;
                }
            } else if (play->sceneId == SCENE_LAKE_HYLIA) {
                if ((LINK_AGE_IN_YEARS == YEARS_ADULT) && !CHECK_QUEST_ITEM(QUEST_MEDALLION_WATER)) {
                    extendedMapIndex = 0x15;
                }
            } else if (play->sceneId == SCENE_GERUDO_VALLEY) {
                if ((LINK_AGE_IN_YEARS == YEARS_ADULT) && !GET_EVENTCHKINF_CARPENTERS_ALL_RESCUED()) {
                    extendedMapIndex = 0x16;
                }
            } else if (play->sceneId == SCENE_GERUDOS_FORTRESS) {
                if (GET_EVENTCHKINF_CARPENTERS_ALL_RESCUED()) {
                    extendedMapIndex = 0x17;
                }
            }
            PRINTF_COLOR_BLUE();
            PRINTF("ＫＫＫ＝%d\n", extendedMapIndex);
            PRINTF_RST();
            sEntranceIconMapIndex = extendedMapIndex;
            DMA_REQUEST_SYNC(interfaceCtx->mapSegment,
                             (uintptr_t)_map_grand_staticSegmentRomStart +
                                 gMapData->owMinimapTexOffset[extendedMapIndex],
                             gMapData->owMinimapTexSize[mapIndex], "../z_map_exp.c", 309);
            interfaceCtx->unk_258 = mapIndex;
            break;
        case SCENE_DEKU_TREE:
        case SCENE_DODONGOS_CAVERN:
        case SCENE_JABU_JABU:
        case SCENE_FOREST_TEMPLE:
        case SCENE_FIRE_TEMPLE:
        case SCENE_WATER_TEMPLE:
        case SCENE_SPIRIT_TEMPLE:
        case SCENE_SHADOW_TEMPLE:
        case SCENE_BOTTOM_OF_THE_WELL:
        case SCENE_ICE_CAVERN:
        case SCENE_DEKU_TREE_BOSS:
        case SCENE_DODONGOS_CAVERN_BOSS:
        case SCENE_JABU_JABU_BOSS:
        case SCENE_FOREST_TEMPLE_BOSS:
        case SCENE_FIRE_TEMPLE_BOSS:
        case SCENE_WATER_TEMPLE_BOSS:
        case SCENE_SPIRIT_TEMPLE_BOSS:
        case SCENE_SHADOW_TEMPLE_BOSS:
            PRINTF_COLOR_YELLOW();
            PRINTF(T("デクの樹ダンジョンＭＡＰ テクスチャＤＭＡ(%x) scene_id_offset=%d  VREG(30)=%d\n",
                     "Deku Tree Dungeon MAP Texture DMA(%x) scene_id_offset=%d  VREG(30)=%d\n"),
                   room, mapIndex, VREG(30));
            PRINTF_RST();

#if PLATFORM_N64
            if ((B_80121220 != NULL) && (B_80121220->unk_28 != NULL) && B_80121220->unk_28(play)) {
            } else {
                DMA_REQUEST_SYNC(play->interfaceCtx.mapSegment,
                                 (uintptr_t)_map_i_staticSegmentRomStart +
                                     ((gMapData->dgnMinimapTexIndexOffset[mapIndex] + room) * MAP_I_TEX_SIZE),
                                 MAP_I_TEX_SIZE, "../z_map_exp.c", UNK_LINE);
            }
#else
            DMA_REQUEST_SYNC(play->interfaceCtx.mapSegment,
                             (uintptr_t)_map_i_staticSegmentRomStart +
                                 ((gMapData->dgnMinimapTexIndexOffset[mapIndex] + room) * MAP_I_TEX_SIZE),
                             MAP_I_TEX_SIZE, "../z_map_exp.c", 346);
#endif

            R_COMPASS_OFFSET_X = gMapData->roomCompassOffsetX[mapIndex][room];
            R_COMPASS_OFFSET_Y = gMapData->roomCompassOffsetY[mapIndex][room];
            Map_SetFloorPalettesData(play, VREG(30));
            PRINTF(T("ＭＡＰ 各階ＯＮチェック\n", "MAP Individual Floor ON Check\n"));
            break;
    }
}

void Map_InitRoomData(PlayState* play, s16 room) {
    s32 mapIndex = gSaveContext.mapIndex;
    InterfaceContext* interfaceCtx = &play->interfaceCtx;

    PRINTF("＊＊＊＊＊＊＊\n＊＊＊＊＊＊＊\nroom_no=%d (%d)(%d)\n＊＊＊＊＊＊＊\n＊＊＊＊＊＊＊\n", room, mapIndex,
           play->sceneId);

    if (room >= 0) {
        switch (play->sceneId) {
            case SCENE_DEKU_TREE:
            case SCENE_DODONGOS_CAVERN:
            case SCENE_JABU_JABU:
            case SCENE_FOREST_TEMPLE:
            case SCENE_FIRE_TEMPLE:
            case SCENE_WATER_TEMPLE:
            case SCENE_SPIRIT_TEMPLE:
            case SCENE_SHADOW_TEMPLE:
            case SCENE_BOTTOM_OF_THE_WELL:
            case SCENE_ICE_CAVERN:
            case SCENE_DEKU_TREE_BOSS:
            case SCENE_DODONGOS_CAVERN_BOSS:
            case SCENE_JABU_JABU_BOSS:
            case SCENE_FOREST_TEMPLE_BOSS:
            case SCENE_FIRE_TEMPLE_BOSS:
            case SCENE_WATER_TEMPLE_BOSS:
            case SCENE_SPIRIT_TEMPLE_BOSS:
            case SCENE_SHADOW_TEMPLE_BOSS:
                gSaveContext.save.info.sceneFlags[mapIndex].rooms |= gBitFlags[room];
                PRINTF("ＲＯＯＭ＿ＩＮＦ＝%d\n", gSaveContext.save.info.sceneFlags[mapIndex].rooms);
                interfaceCtx->mapRoomNum = room;
                interfaceCtx->unk_25A = mapIndex;
                Map_SetPaletteData(play, room);
                PRINTF_COLOR_YELLOW();
                PRINTF(T("部屋部屋＝%d\n", "Room Room = %d\n"), room);
                PRINTF_RST();
                Map_InitData(play, room);
                break;
        }
    } else {
        interfaceCtx->mapRoomNum = 0;
    }

    if (gSaveContext.sunsSongState != SUNSSONG_SPEED_TIME) {
        gSaveContext.sunsSongState = SUNSSONG_INACTIVE;
    }
}

void Map_Destroy(PlayState* play) {
    MapMark_ClearPointers(play);

#if PLATFORM_N64
    if ((B_80121220 != NULL) && (B_80121220->unk_24 != NULL)) {
        B_80121220->unk_24();
    }
    if ((B_80121220 != NULL) && (B_80121220->unk_1C != NULL)) {
        B_80121220->unk_1C(&gMapData);
    }
#endif

    gMapData = NULL;
}

void Map_Init(PlayState* play) {
    s32 mapIndex = gSaveContext.mapIndex;
    InterfaceContext* interfaceCtx = &play->interfaceCtx;

    gMapData = &gMapDataTable;

#if PLATFORM_N64
    if ((B_80121220 != NULL) && (B_80121220->unk_18 != NULL)) {
        B_80121220->unk_18(&gMapData);
    }
#endif

    interfaceCtx->unk_258 = -1;
    interfaceCtx->unk_25A = -1;

    interfaceCtx->mapSegment = GAME_STATE_ALLOC(&play->state, 0x1000, "../z_map_exp.c", 457);
    PRINTF(T("\n\n\nＭＡＰ テクスチャ初期化   scene_data_ID=%d\nmapSegment=%x\n\n",
             "\n\n\nMAP texture initialization   scene_data_ID=%d\nmapSegment=%x\n\n"),
           play->sceneId, interfaceCtx->mapSegment);
    ASSERT(interfaceCtx->mapSegment != NULL, "parameter->mapSegment != NULL", "../z_map_exp.c", 459);

    switch (play->sceneId) {
        case SCENE_HYRULE_FIELD:
        case SCENE_KAKARIKO_VILLAGE:
        case SCENE_GRAVEYARD:
        case SCENE_ZORAS_RIVER:
        case SCENE_KOKIRI_FOREST:
        case SCENE_SACRED_FOREST_MEADOW:
        case SCENE_LAKE_HYLIA:
        case SCENE_ZORAS_DOMAIN:
        case SCENE_ZORAS_FOUNTAIN:
        case SCENE_GERUDO_VALLEY:
        case SCENE_LOST_WOODS:
        case SCENE_DESERT_COLOSSUS:
        case SCENE_GERUDOS_FORTRESS:
        case SCENE_HAUNTED_WASTELAND:
        case SCENE_HYRULE_CASTLE:
        case SCENE_DEATH_MOUNTAIN_TRAIL:
        case SCENE_DEATH_MOUNTAIN_CRATER:
        case SCENE_GORON_CITY:
        case SCENE_LON_LON_RANCH:
        case SCENE_OUTSIDE_GANONS_CASTLE:
            mapIndex = play->sceneId - SCENE_HYRULE_FIELD;
            R_MAP_INDEX = gSaveContext.mapIndex = mapIndex;
            R_COMPASS_SCALE_X = gMapData->owCompassInfo[mapIndex][0];
            R_COMPASS_SCALE_Y = gMapData->owCompassInfo[mapIndex][1];
            R_COMPASS_OFFSET_X = gMapData->owCompassInfo[mapIndex][2];
            R_COMPASS_OFFSET_Y = gMapData->owCompassInfo[mapIndex][3];
            Map_InitData(play, mapIndex);
            R_OW_MINIMAP_X = gMapData->owMinimapPosX[mapIndex];
            R_OW_MINIMAP_Y = gMapData->owMinimapPosY[mapIndex];
            break;
        case SCENE_DEKU_TREE:
        case SCENE_DODONGOS_CAVERN:
        case SCENE_JABU_JABU:
        case SCENE_FOREST_TEMPLE:
        case SCENE_FIRE_TEMPLE:
        case SCENE_WATER_TEMPLE:
        case SCENE_SPIRIT_TEMPLE:
        case SCENE_SHADOW_TEMPLE:
        case SCENE_BOTTOM_OF_THE_WELL:
        case SCENE_ICE_CAVERN:
        case SCENE_GANONS_TOWER:
        case SCENE_GERUDO_TRAINING_GROUND:
        case SCENE_THIEVES_HIDEOUT:
        case SCENE_INSIDE_GANONS_CASTLE:
        case SCENE_GANONS_TOWER_COLLAPSE_INTERIOR:
        case SCENE_INSIDE_GANONS_CASTLE_COLLAPSE:
        case SCENE_TREASURE_BOX_SHOP:
        case SCENE_DEKU_TREE_BOSS:
        case SCENE_DODONGOS_CAVERN_BOSS:
        case SCENE_JABU_JABU_BOSS:
        case SCENE_FOREST_TEMPLE_BOSS:
        case SCENE_FIRE_TEMPLE_BOSS:
        case SCENE_WATER_TEMPLE_BOSS:
        case SCENE_SPIRIT_TEMPLE_BOSS:
        case SCENE_SHADOW_TEMPLE_BOSS:
            mapIndex = (play->sceneId >= SCENE_DEKU_TREE_BOSS) ? play->sceneId - SCENE_DEKU_TREE_BOSS : play->sceneId;
            R_MAP_INDEX = gSaveContext.mapIndex = mapIndex;
            if ((play->sceneId <= SCENE_ICE_CAVERN) || (play->sceneId >= SCENE_DEKU_TREE_BOSS)) {
                R_COMPASS_SCALE_X = gMapData->dgnCompassInfo[mapIndex][0];
                R_COMPASS_SCALE_Y = gMapData->dgnCompassInfo[mapIndex][1];
                R_COMPASS_OFFSET_X = gMapData->dgnCompassInfo[mapIndex][2];
                R_COMPASS_OFFSET_Y = gMapData->dgnCompassInfo[mapIndex][3];
                R_MAP_TEX_INDEX = R_MAP_TEX_INDEX_BASE = gMapData->dgnTexIndexBase[mapIndex];
#if PLATFORM_N64
                if ((B_80121220 != NULL) && (B_80121220->unk_20 != NULL)) {
                    B_80121220->unk_20(gMapData);
                }
#endif
                Map_InitRoomData(play, play->roomCtx.curRoom.num);
                MapMark_Init(play);
            }
            break;
    }
}

void Minimap_DrawCompassIcons(PlayState* play) {
    s32 pad;
    Player* player = GET_PLAYER(play);
    s16 tempX, tempZ;

    OPEN_DISPS(play->state.gfxCtx, "../z_map_exp.c", 565);

    if (play->interfaceCtx.minimapAlpha >= 0xAA) {
        Gfx_SetupDL_42Overlay(play->state.gfxCtx);

        gSPMatrix(OVERLAY_DISP++, &gIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                          PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
        gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
        gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

        tempX = player->actor.world.pos.x;
        tempZ = player->actor.world.pos.z;
        tempX /= R_COMPASS_SCALE_X;
        tempZ /= R_COMPASS_SCALE_Y;
        Matrix_Translate((R_COMPASS_OFFSET_X + tempX) / 10.0f, (R_COMPASS_OFFSET_Y - tempZ) / 10.0f, 0.0f, MTXMODE_NEW);
        Matrix_Scale(0.4f, 0.4f, 0.4f, MTXMODE_APPLY);
        Matrix_RotateX(-1.6f, MTXMODE_APPLY);
        tempX = (0x7FFF - player->actor.shape.rot.y) / 0x400;
        Matrix_RotateY(tempX / 10.0f, MTXMODE_APPLY);
        MATRIX_FINALIZE_AND_LOAD(OVERLAY_DISP++, play->state.gfxCtx, "../z_map_exp.c", 585);

        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 200, 255, 0, 255);
        gSPDisplayList(OVERLAY_DISP++, gCompassArrowDL);

        tempX = sPlayerInitialPosX;
        tempZ = sPlayerInitialPosZ;
        tempX /= R_COMPASS_SCALE_X;
        tempZ /= R_COMPASS_SCALE_Y;
        Matrix_Translate((R_COMPASS_OFFSET_X + tempX) / 10.0f, (R_COMPASS_OFFSET_Y - tempZ) / 10.0f, 0.0f, MTXMODE_NEW);
        Matrix_Scale(VREG(9) / 100.0f, VREG(9) / 100.0f, VREG(9) / 100.0f, MTXMODE_APPLY);
        Matrix_RotateX(VREG(52) / 10.0f, MTXMODE_APPLY);
        Matrix_RotateY(sPlayerInitialDirection / 10.0f, MTXMODE_APPLY);
        MATRIX_FINALIZE_AND_LOAD(OVERLAY_DISP++, play->state.gfxCtx, "../z_map_exp.c", 603);

        gDPSetPrimColor(OVERLAY_DISP++, 0, 0xFF, 200, 0, 0, 255);
        gSPDisplayList(OVERLAY_DISP++, gCompassArrowDL);
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_map_exp.c", 607);
}

#ifdef __3DS__
#include "port_minimap.h"
void MapMark_Export(PlayState* play);

/* PORT: hand the minimap to the touch screen (see port_minimap.h). Same data, positions and visibility
 * rules as the drawing code below; returns true when the top screen should not draw it. */
static s32 Minimap_ExportToBottom(PlayState* play, s32 dungeon) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    s32 mapIndex = gSaveContext.mapIndex;
    Player* player = GET_PLAYER(play);
    PortMinimap* m = &gPortMinimap;

    if (!gPortMinimapOnBottom) {
        return false;
    }
    if (R_MINIMAP_DISABLED || (interfaceCtx->minimapAlpha == 0)) {
        return true; /* serial not bumped: the panel shows no map */
    }
    m->tex = NULL;
    m->numIcons = 0;
    m->numMarks = 0;
    if (dungeon) {
        m->compass = CHECK_DUNGEON_ITEM(DUNGEON_COMPASS, mapIndex);
        if (CHECK_DUNGEON_ITEM(DUNGEON_MAP, mapIndex)) {
            m->tex = interfaceCtx->mapSegment;
            m->fmt = PORT_MINIMAP_I4;
            m->r = 100, m->g = 255, m->b = 255;
            m->w = MAP_I_TEX_WIDTH, m->h = MAP_I_TEX_HEIGHT;
            m->x = R_DGN_MINIMAP_X, m->y = R_DGN_MINIMAP_Y;
        } else {
            /* no map item: the frame still anchors the compass arrows */
            m->w = MAP_I_TEX_WIDTH, m->h = MAP_I_TEX_HEIGHT;
            m->x = R_DGN_MINIMAP_X, m->y = R_DGN_MINIMAP_Y;
        }
        if (m->compass) {
            MapMark_Export(play);
        }
    } else {
        m->compass = true;
        m->tex = interfaceCtx->mapSegment;
        m->fmt = PORT_MINIMAP_IA4;
        m->r = R_MINIMAP_COLOR(0), m->g = R_MINIMAP_COLOR(1), m->b = R_MINIMAP_COLOR(2);
        m->w = gMapData->owMinimapWidth[mapIndex], m->h = gMapData->owMinimapHeight[mapIndex];
        m->x = R_OW_MINIMAP_X, m->y = R_OW_MINIMAP_Y;
        if (((play->sceneId != SCENE_KAKARIKO_VILLAGE) && (play->sceneId != SCENE_KOKIRI_FOREST) &&
             (play->sceneId != SCENE_ZORAS_FOUNTAIN)) ||
            (LINK_AGE_IN_YEARS != YEARS_ADULT)) {
            if ((gMapData->owEntranceFlag[sEntranceIconMapIndex] == 0xFFFF) ||
                ((gMapData->owEntranceFlag[sEntranceIconMapIndex] != 0xFFFF) &&
                 (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] & gBitFlags[gMapData->owEntranceFlag[mapIndex]]))) {
                m->iconX[m->numIcons] = gMapData->owEntranceIconPosX[sEntranceIconMapIndex];
                m->iconY[m->numIcons] = gMapData->owEntranceIconPosY[sEntranceIconMapIndex];
                m->numIcons++;
            }
        }
        if ((play->sceneId == SCENE_ZORAS_FOUNTAIN) &&
            (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] & gBitFlags[INFTABLE_1A9_SHIFT])) {
            m->iconX[m->numIcons] = 270;
            m->iconY[m->numIcons] = 154;
            m->numIcons++;
        }
    }
    if (m->compass) {
        /* Minimap_DrawCompassIcons: ortho overlay, 1 unit = 1 pixel, origin at the screen center */
        s16 tempX = player->actor.world.pos.x;
        s16 tempZ = player->actor.world.pos.z;

        tempX /= R_COMPASS_SCALE_X;
        tempZ /= R_COMPASS_SCALE_Y;
        m->playerX = 160.0f + (R_COMPASS_OFFSET_X + tempX) / 10.0f;
        m->playerY = 120.0f - (R_COMPASS_OFFSET_Y - tempZ) / 10.0f;
        m->playerYaw = player->actor.shape.rot.y;
        tempX = sPlayerInitialPosX;
        tempZ = sPlayerInitialPosZ;
        tempX /= R_COMPASS_SCALE_X;
        tempZ /= R_COMPASS_SCALE_Y;
        m->startX = 160.0f + (R_COMPASS_OFFSET_X + tempX) / 10.0f;
        m->startY = 120.0f - (R_COMPASS_OFFSET_Y - tempZ) / 10.0f;
        m->startYaw = 0x7FFF - sPlayerInitialDirection * 0x400;
    }
    m->serial++;
    return true;
}
#endif

void Minimap_Draw(PlayState* play) {
    s32 pad[2];
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    s32 mapIndex = gSaveContext.mapIndex;

    OPEN_DISPS(play->state.gfxCtx, "../z_map_exp.c", 626);

    if (play->pauseCtx.state <= PAUSE_STATE_INIT) {
        switch (play->sceneId) {
            case SCENE_DEKU_TREE:
            case SCENE_DODONGOS_CAVERN:
            case SCENE_JABU_JABU:
            case SCENE_FOREST_TEMPLE:
            case SCENE_FIRE_TEMPLE:
            case SCENE_WATER_TEMPLE:
            case SCENE_SPIRIT_TEMPLE:
            case SCENE_SHADOW_TEMPLE:
            case SCENE_BOTTOM_OF_THE_WELL:
            case SCENE_ICE_CAVERN:
#ifdef __3DS__
                if (Minimap_ExportToBottom(play, true)) {
                } else
#endif
                if (!R_MINIMAP_DISABLED) {
                    Gfx_SetupDL_39Overlay(play->state.gfxCtx);
                    gDPSetCombineLERP(OVERLAY_DISP++, 1, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0, 1, 0, PRIMITIVE, 0,
                                      TEXEL0, 0, PRIMITIVE, 0);

                    if (CHECK_DUNGEON_ITEM(DUNGEON_MAP, mapIndex)) {
                        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 100, 255, 255, interfaceCtx->minimapAlpha);

                        gDPLoadTextureBlock_4b(OVERLAY_DISP++, interfaceCtx->mapSegment, G_IM_FMT_I, MAP_I_TEX_WIDTH,
                                               MAP_I_TEX_HEIGHT, 0, G_TX_NOMIRROR | G_TX_WRAP,
                                               G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD,
                                               G_TX_NOLOD);

                        gSPTextureRectangle(OVERLAY_DISP++, R_DGN_MINIMAP_X << 2, R_DGN_MINIMAP_Y << 2,
                                            (R_DGN_MINIMAP_X + MAP_I_TEX_WIDTH) << 2,
                                            (R_DGN_MINIMAP_Y + MAP_I_TEX_HEIGHT) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10,
                                            1 << 10);
                    }

                    if (CHECK_DUNGEON_ITEM(DUNGEON_COMPASS, mapIndex)) {
                        Minimap_DrawCompassIcons(play); // Draw icons for the player spawn and current position
                        Gfx_SetupDL_39Overlay(play->state.gfxCtx);
                        MapMark_Draw(play);
                    }
                }

                if (CHECK_BTN_ALL(play->state.input[0].press.button, BTN_L) && !Play_InCsMode(play)) {
                    PRINTF("Game_play_demo_mode_check=%d\n", Play_InCsMode(play));
                    // clang-format off
                    if (!R_MINIMAP_DISABLED) { SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_UP);
                    } else {
                        SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_DOWN);
                    }
                    // clang-format on
                    R_MINIMAP_DISABLED ^= 1;
                }

                break;
            case SCENE_HYRULE_FIELD:
            case SCENE_KAKARIKO_VILLAGE:
            case SCENE_GRAVEYARD:
            case SCENE_ZORAS_RIVER:
            case SCENE_KOKIRI_FOREST:
            case SCENE_SACRED_FOREST_MEADOW:
            case SCENE_LAKE_HYLIA:
            case SCENE_ZORAS_DOMAIN:
            case SCENE_ZORAS_FOUNTAIN:
            case SCENE_GERUDO_VALLEY:
            case SCENE_LOST_WOODS:
            case SCENE_DESERT_COLOSSUS:
            case SCENE_GERUDOS_FORTRESS:
            case SCENE_HAUNTED_WASTELAND:
            case SCENE_HYRULE_CASTLE:
            case SCENE_DEATH_MOUNTAIN_TRAIL:
            case SCENE_DEATH_MOUNTAIN_CRATER:
            case SCENE_GORON_CITY:
            case SCENE_LON_LON_RANCH:
            case SCENE_OUTSIDE_GANONS_CASTLE:
#ifdef __3DS__
                if (Minimap_ExportToBottom(play, false)) {
                } else
#endif
                if (!R_MINIMAP_DISABLED) {
                    Gfx_SetupDL_39Overlay(play->state.gfxCtx);

                    gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
                    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, R_MINIMAP_COLOR(0), R_MINIMAP_COLOR(1), R_MINIMAP_COLOR(2),
                                    interfaceCtx->minimapAlpha);

                    gDPLoadTextureBlock_4b(OVERLAY_DISP++, interfaceCtx->mapSegment, G_IM_FMT_IA,
                                           gMapData->owMinimapWidth[mapIndex], gMapData->owMinimapHeight[mapIndex], 0,
                                           G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK,
                                           G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

                    gSPTextureRectangle(OVERLAY_DISP++, R_OW_MINIMAP_X << 2, R_OW_MINIMAP_Y << 2,
                                        (R_OW_MINIMAP_X + gMapData->owMinimapWidth[mapIndex]) << 2,
                                        (R_OW_MINIMAP_Y + gMapData->owMinimapHeight[mapIndex]) << 2, G_TX_RENDERTILE, 0,
                                        0, 1 << 10, 1 << 10);

                    if (((play->sceneId != SCENE_KAKARIKO_VILLAGE) && (play->sceneId != SCENE_KOKIRI_FOREST) &&
                         (play->sceneId != SCENE_ZORAS_FOUNTAIN)) ||
                        (LINK_AGE_IN_YEARS != YEARS_ADULT)) {
                        if ((gMapData->owEntranceFlag[sEntranceIconMapIndex] == 0xFFFF) ||
                            ((gMapData->owEntranceFlag[sEntranceIconMapIndex] != 0xFFFF) &&
                             (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] &
                              gBitFlags[gMapData->owEntranceFlag[mapIndex]]))) {

                            gDPLoadTextureBlock(OVERLAY_DISP++, gMapDungeonEntranceIconTex, G_IM_FMT_RGBA, G_IM_SIZ_16b,
                                                8, 8, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP,
                                                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

                            gSPTextureRectangle(OVERLAY_DISP++,
                                                gMapData->owEntranceIconPosX[sEntranceIconMapIndex] << 2,
                                                gMapData->owEntranceIconPosY[sEntranceIconMapIndex] << 2,
                                                (gMapData->owEntranceIconPosX[sEntranceIconMapIndex] + 8) << 2,
                                                (gMapData->owEntranceIconPosY[sEntranceIconMapIndex] + 8) << 2,
                                                G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
                        }
                    }

                    if ((play->sceneId == SCENE_ZORAS_FOUNTAIN) &&
                        (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] & gBitFlags[INFTABLE_1A9_SHIFT])) {
                        gDPLoadTextureBlock(OVERLAY_DISP++, gMapDungeonEntranceIconTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8,
                                            8, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK,
                                            G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

                        gSPTextureRectangle(OVERLAY_DISP++, 270 << 2, 154 << 2, 278 << 2, 162 << 2, G_TX_RENDERTILE, 0,
                                            0, 1 << 10, 1 << 10);
                    }

                    Minimap_DrawCompassIcons(play); // Draw icons for the player spawn and current position
                }

                if (CHECK_BTN_ALL(play->state.input[0].press.button, BTN_L) && !Play_InCsMode(play)) {
                    // clang-format off
                    if (!R_MINIMAP_DISABLED) { SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_UP);
                    } else {
                        SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_DOWN);
                    }
                    // clang-format on
                    R_MINIMAP_DISABLED ^= 1;
                }

                break;
        }
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_map_exp.c", 782);
}

s16 Map_GetFloorTextIndexOffset(s32 mapIndex, s32 floor) {
    return gMapData->floorTexIndexOffset[mapIndex][floor];
}

#ifdef __3DS__
PortFloorMap gPortFloorMap = { 0, 0, { 0 }, { 0 }, 0, -1, NULL, { 0 }, -1 };
volatile int gPortFloorReq = -1;

/* PORT (2026-10-07): the floor list and, on request, one floor's map for the touch screen (port_minimap.h). The
 * floors and the map are the pause map page's (z_kaleido_map.c, KaleidoScope_UpdateDungeonMap): a floor is listed
 * once visited, or all of them with the Map; its rooms are coloured when visited, outlined with the Map. */
static void Map_ExportFloors(PlayState* play, s32 dungeonIndex) {
    static u64 sFloorTex[2 * ALIGN16(MAP_48x85_TEX_SIZE) / sizeof(u64)];
    static s32 sLoadedTexIndex = -1;
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    PortFloorMap* f = &gPortFloorMap;
    s32 mapIndex = gSaveContext.mapIndex;
    s32 req = gPortFloorReq;
    s32 i, n = 0;

    f->curSlot = VREG(30);
    if (interfaceCtx->unk_25A >= 0) {
        for (i = 0; i < PORT_FLOOR_MAX; i++) {
            u8 id = gMapData->floorID[interfaceCtx->unk_25A][i];

            if (id != 0 && ((gSaveContext.save.info.sceneFlags[mapIndex].floors & gBitFlags[i]) ||
                            CHECK_DUNGEON_ITEM(DUNGEON_MAP, mapIndex) || i == f->curSlot)) {
                f->slot[n] = i;
                f->label[n] = id;
                n++;
            }
        }
    }
    f->numFloors = n;
    if (req >= 0 && req < PORT_FLOOR_MAX) {
        s32 texIndex = R_MAP_TEX_INDEX_BASE + gMapData->floorTexIndexOffset[dungeonIndex][req];
        char savedPal[sizeof(interfaceCtx->mapPalette)];
        s16 savedIndex = interfaceCtx->mapPaletteIndex;

        if (texIndex != sLoadedTexIndex) {
            DMA_REQUEST_SYNC(sFloorTex, (uintptr_t)_map_48x85_staticSegmentRomStart + texIndex * MAP_48x85_TEX_SIZE,
                             MAP_48x85_TEX_SIZE, "../z_map_exp.c", __LINE__);
            DMA_REQUEST_SYNC((u8*)sFloorTex + ALIGN16(MAP_48x85_TEX_SIZE),
                             (uintptr_t)_map_48x85_staticSegmentRomStart + (texIndex + 1) * MAP_48x85_TEX_SIZE,
                             MAP_48x85_TEX_SIZE, "../z_map_exp.c", __LINE__);
            sLoadedTexIndex = texIndex;
        }
        /* the game's own palette builder, on a copy: the pause page and the HUD keep theirs */
        for (i = 0; i < (s32)sizeof(savedPal); i++) {
            savedPal[i] = interfaceCtx->mapPalette[i];
        }
        Map_SetFloorPalettesData(play, req);
        for (i = 0; i < 16; i++) {
            f->pal[i] = ((u8)interfaceCtx->mapPalette[i * 2] << 8) | (u8)interfaceCtx->mapPalette[i * 2 + 1];
        }
        for (i = 0; i < (s32)sizeof(savedPal); i++) {
            interfaceCtx->mapPalette[i] = savedPal[i];
        }
        interfaceCtx->mapPaletteIndex = savedIndex;
        f->curRoomIndex = (req == f->curSlot) ? savedIndex : -1;
        f->tex = (const unsigned char*)sFloorTex;
        f->mapSlot = req;
    }
    f->serial++;
}
#endif

void Map_Update(PlayState* play) {
    static s16 sLastRoomNum = 99;
    Player* player = GET_PLAYER(play);
    s32 mapIndex = gSaveContext.mapIndex;
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    s16 floor;
    s16 i;

    if (!IS_PAUSED(&play->pauseCtx)) {
        switch (play->sceneId) {
            case SCENE_DEKU_TREE:
            case SCENE_DODONGOS_CAVERN:
            case SCENE_JABU_JABU:
            case SCENE_FOREST_TEMPLE:
            case SCENE_FIRE_TEMPLE:
            case SCENE_WATER_TEMPLE:
            case SCENE_SPIRIT_TEMPLE:
            case SCENE_SHADOW_TEMPLE:
            case SCENE_BOTTOM_OF_THE_WELL:
            case SCENE_ICE_CAVERN:
                interfaceCtx->mapPalette[30] = 0;
                if (CHECK_DUNGEON_ITEM(DUNGEON_MAP, mapIndex)) {
                    interfaceCtx->mapPalette[31] = 1;
                } else {
                    interfaceCtx->mapPalette[31] = 0;
                }

                for (floor = 0; floor < 8; floor++) {
                    if (player->actor.world.pos.y > gMapData->floorCoordY[mapIndex][floor]) {
                        break;
                    }
                }

                gSaveContext.save.info.sceneFlags[mapIndex].floors |= gBitFlags[floor];
                VREG(30) = floor;
                if (R_MAP_TEX_INDEX != (R_MAP_TEX_INDEX_BASE + Map_GetFloorTextIndexOffset(mapIndex, floor))) {
                    R_MAP_TEX_INDEX = R_MAP_TEX_INDEX_BASE + Map_GetFloorTextIndexOffset(mapIndex, floor);
                }

                if (interfaceCtx->mapRoomNum != sLastRoomNum) {
                    PRINTF(T("現在階＝%d  現在部屋＝%x  部屋数＝%d\n",
                             "Current floor = %d  Current room = %x  Number of rooms = %d\n"),
                           floor, interfaceCtx->mapRoomNum, gMapData->switchEntryCount[mapIndex]);
                    sLastRoomNum = interfaceCtx->mapRoomNum;
                }

                for (i = 0; i < gMapData->switchEntryCount[mapIndex]; i++) {
                    if ((interfaceCtx->mapRoomNum == gMapData->switchFromRoom[mapIndex][i]) &&
                        (floor == gMapData->switchFromFloor[mapIndex][i])) {
                        interfaceCtx->mapRoomNum = gMapData->switchToRoom[mapIndex][i];
                        PRINTF_COLOR_YELLOW();
                        PRINTF(T("階層切替＝%x\n", "Layer switching = %x\n"), interfaceCtx->mapRoomNum);
                        PRINTF_RST();
                        Map_InitData(play, interfaceCtx->mapRoomNum);
                        gSaveContext.sunsSongState = SUNSSONG_INACTIVE;
                        Map_SavePlayerInitialInfo(play);
                    }
                }

                VREG(10) = interfaceCtx->mapRoomNum;
#ifdef __3DS__
                Map_ExportFloors(play, mapIndex);
#endif
                break;
            case SCENE_DEKU_TREE_BOSS:
            case SCENE_DODONGOS_CAVERN_BOSS:
            case SCENE_JABU_JABU_BOSS:
            case SCENE_FOREST_TEMPLE_BOSS:
            case SCENE_FIRE_TEMPLE_BOSS:
            case SCENE_WATER_TEMPLE_BOSS:
            case SCENE_SPIRIT_TEMPLE_BOSS:
            case SCENE_SHADOW_TEMPLE_BOSS:
                VREG(30) = gMapData->bossFloor[play->sceneId - SCENE_DEKU_TREE_BOSS];
                R_MAP_TEX_INDEX = R_MAP_TEX_INDEX_BASE +
                                  gMapData->floorTexIndexOffset[play->sceneId - SCENE_DEKU_TREE_BOSS][VREG(30)];
#ifdef __3DS__
                Map_ExportFloors(play, play->sceneId - SCENE_DEKU_TREE_BOSS);
#endif
                break;
        }
    }
}

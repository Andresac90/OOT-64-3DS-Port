/*
 * File: z_opening.c
 * Overlay: ovl_opening
 * Description: Initializes the game into the title screen
 */

#include "gfx.h"
#include "regs.h"
#include "sys_matrix.h"
#include "title_setup_state.h"
#include "game.h"
#include "play_state.h"
#include "save.h"
#include "sram.h"
#include "view.h"
/* #include "file_select_state.h"  // restore for PORT_FS_TEST */

void TitleSetup_SetupTitleScreen(TitleSetupState* this) {
#ifdef __3DS__
    // Boot bypass: cutscene system works; D7 heap-vs-segment-8 collision FIXED (seg_addr +
    // PortSegmentedToVirtual) so the file-select no longer runs away (0 guards), but its
    // content still renders black (separate textures/combiner/draw issue). Boot into playable
    // Hyrule Field. To test the file-select: restore file_select_state.h + GAMEMODE_FILE_SELECT
    // + SET_NEXT_GAMESTATE(FileSelect_Init, FileSelectState).
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    this->state.running = false;
    gSaveContext.save.linkAge = LINK_AGE_ADULT;
    Sram_InitDebugSave();
    gSaveContext.save.cutsceneIndex = 0; // < 0xFFF0 => normal (interactive) scene layer
    gSaveContext.sceneLayer = 0;
    gSaveContext.fileNum = 0;
#ifdef PORT_START_ENTRANCE
    // Test hook (build with GAME_EXTRA=-DPORT_START_ENTRANCE=<entrance>): boot a child Link into a
    // specific entrance, e.g. 0xB7 Bazaar / 0xB1 Market to exercise pre-rendered (S2DEX) rooms.
    gSaveContext.save.linkAge = LINK_AGE_CHILD;
    gSaveContext.save.entranceIndex = PORT_START_ENTRANCE;
#endif
    SET_NEXT_GAMESTATE(&this->state, Play_Init, PlayState);
#else
    gSaveContext.gameMode = GAMEMODE_TITLE_SCREEN;
    this->state.running = false;
    gSaveContext.save.linkAge = LINK_AGE_ADULT;
    Sram_InitDebugSave();
    gSaveContext.save.cutsceneIndex = CS_INDEX_3;
    gSaveContext.sceneLayer = GET_CUTSCENE_LAYER(CS_INDEX_3);
    SET_NEXT_GAMESTATE(&this->state, Play_Init, PlayState);
#endif
}

void func_80803C5C(TitleSetupState* this) {
}

void TitleSetup_Main(GameState* thisx) {
    TitleSetupState* this = (TitleSetupState*)thisx;

    Gfx_SetupFrame(this->state.gfxCtx, 0, 0, 0);
    TitleSetup_SetupTitleScreen(this);
    func_80803C5C(this);
}

void TitleSetup_Destroy(GameState* thisx) {
}

void TitleSetup_Init(GameState* thisx) {
    TitleSetupState* this = (TitleSetupState*)thisx;

    R_UPDATE_RATE = 1;
    Matrix_Init(&this->state);
    View_Init(&this->view, this->state.gfxCtx);
    this->state.main = TitleSetup_Main;
    this->state.destroy = TitleSetup_Destroy;
}

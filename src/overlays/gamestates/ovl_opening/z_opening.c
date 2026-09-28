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
#if defined(__3DS__) && !defined(PORT_NORMAL_BOOT) && \
    (defined(PORT_STATEDUMP) || defined(PORT_START_ENTRANCE) || defined(PORT_DEBUG_BOOT))
    // PORT: debug boot straight into Play with the debug save, for tools (tools/statediff tours set
    // PORT_STATEDUMP / PORT_START_ENTRANCE) or GAME_EXTRA=-DPORT_DEBUG_BOOT. Normal builds run the real
    // flow (console logo, title screen, file select): it matches the N64 state for state
    // (tools/statediff/bootflow.py title, 2026-09-28).
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
#ifdef PORT_START_AGE
    gSaveContext.save.linkAge = PORT_START_AGE; // tools/statediff scene tour: LINK_AGE_ADULT (0) / CHILD (1)
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

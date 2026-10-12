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
#if defined(PORT_START_FRESH) || defined(PORT_MAKE_FULL_SAVE)
#include "array_count.h"
#include "inventory.h"
#endif
#ifdef PORT_MAKE_FULL_SAVE
#include "item.h"
#include "scene.h"
#endif
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
#ifdef PORT_START_CS
    gSaveContext.save.cutsceneIndex = PORT_START_CS; /* e.g. 0xFFF3 with entrance 0xEE: the Deku Tree calls Navi */
#endif
#ifdef PORT_START_DAYTIME
    gSaveContext.save.dayTime = PORT_START_DAYTIME; // e.g. 0x8000 = noon (tools/make_showcase.py)
#endif
#ifdef PORT_START_AGE
    gSaveContext.save.linkAge = PORT_START_AGE; // tools/statediff scene tour: LINK_AGE_ADULT (0) / CHILD (1)
#endif
#ifdef PORT_START_FRESH
    {
        // tools/make_showcase.py: a new file's inventory (3 hearts, no items, no magic) with only the Kokiri Sword
        // and the Deku Shield, as early in the game; the debug save's story flags and name stay (no first-visit
        // cutscenes in the shots)
        static SaveInfo sDebugInfo;
        int i;

        sDebugInfo = gSaveContext.save.info;
        Sram_InitNewSave();
        for (i = 0; i < ARRAY_COUNT(sDebugInfo.eventChkInf); i++) {
            gSaveContext.save.info.eventChkInf[i] = sDebugInfo.eventChkInf[i];
        }
        for (i = 0; i < ARRAY_COUNT(sDebugInfo.itemGetInf); i++) {
            gSaveContext.save.info.itemGetInf[i] = sDebugInfo.itemGetInf[i];
        }
        for (i = 0; i < ARRAY_COUNT(sDebugInfo.infTable); i++) {
            gSaveContext.save.info.infTable[i] = sDebugInfo.infTable[i];
        }
        for (i = 0; i < ARRAY_COUNT(sDebugInfo.playerData.playerName); i++) {
            gSaveContext.save.info.playerData.playerName[i] = sDebugInfo.playerData.playerName[i];
        }
        gSaveContext.save.info.inventory.equipment |= OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_KOKIRI) |
                                                      OWNED_EQUIP_FLAG(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_DEKU);
        gSaveContext.save.info.equips.buttonItems[0] = ITEM_SWORD_KOKIRI;
        Inventory_ChangeEquipment(EQUIP_TYPE_SWORD, EQUIP_VALUE_SWORD_KOKIRI);
        Inventory_ChangeEquipment(EQUIP_TYPE_SHIELD, EQUIP_VALUE_SHIELD_DEKU);
    }
#endif
#ifdef PORT_MAKE_FULL_SAVE
    {
        /* Tool build (GAME_EXTRA=-DPORT_DEBUG_BOOT -DPORT_MAKE_FULL_SAVE): writes FILE 2 of sdmc:/3ds/oot/save.bin as a
         * finished game, for visiting late dungeons, then boots on with saving off. File 1 is left as it is (the SRAM
         * layer loads save.bin first; Sram_WriteSave writes only file 2's slot and its backup). The debug save already
         * has every item, song, medallion and stone, and each dungeon's map, compass, boss key and keys. */
        extern void PortSram_FlushNow(void);
        extern void PortDbg(const char*);
        SramContext sramCtx;
        int u;

        gSaveContext.save.linkAge = LINK_AGE_ADULT;
        gSaveContext.save.dayTime = CLOCK_TIME(12, 0);
        gSaveContext.save.info.playerData.healthCapacity = gSaveContext.save.info.playerData.health = 20 * 16;
        gSaveContext.save.info.playerData.isMagicAcquired = true;
        gSaveContext.save.info.playerData.isDoubleMagicAcquired = true;
        gSaveContext.save.info.playerData.magicLevel = 2;
        gSaveContext.save.info.playerData.magic = 0x60;
        gSaveContext.save.info.playerData.isDoubleDefenseAcquired = true;
        gSaveContext.save.info.inventory.defenseHearts = 20;
        gSaveContext.save.info.inventory.gsTokens = 100;
        for (u = UPG_QUIVER; u <= UPG_DEKU_NUTS; u++) {
            Inventory_ChangeUpgrade(u, u == UPG_SCALE ? 2 : 3);
        }
        gSaveContext.save.info.playerData.rupees = 500;
        gSaveContext.save.info.equips.buttonItems[0] = ITEM_SWORD_MASTER;
        Inventory_ChangeEquipment(EQUIP_TYPE_SWORD, EQUIP_VALUE_SWORD_MASTER);
        Inventory_ChangeEquipment(EQUIP_TYPE_SHIELD, EQUIP_VALUE_SHIELD_HYLIAN);
        SET_EVENTCHKINF(EVENTCHKINF_OPENED_DOOR_OF_TIME);
        SET_EVENTCHKINF(EVENTCHKINF_REVEALED_MASTER_SWORD);
        SET_EVENTCHKINF(EVENTCHKINF_EPONA_OBTAINED);
        SET_EVENTCHKINF(EVENTCHKINF_CARPENTER_0_RESCUED);
        SET_EVENTCHKINF(EVENTCHKINF_CARPENTER_1_RESCUED);
        SET_EVENTCHKINF(EVENTCHKINF_CARPENTER_2_RESCUED);
        SET_EVENTCHKINF(EVENTCHKINF_CARPENTER_3_RESCUED);
        SET_EVENTCHKINF(EVENTCHKINF_DRAINED_WELL);
        SET_EVENTCHKINF(EVENTCHKINF_RESTORED_LAKE_HYLIA);
        SET_EVENTCHKINF(EVENTCHKINF_CREATED_RAINBOW_BRIDGE);
        SET_EVENTCHKINF(EVENTCHKINF_OPENED_JABU_JABU);
        SET_EVENTCHKINF(EVENTCHKINF_GAVE_LETTER_TO_KING_ZORA);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_GOHMA_BATTLE);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_KING_DODONGO_BATTLE);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_BARINADE_BATTLE);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_PHANTOM_GANON_BATTLE);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_VOLVAGIA_BATTLE);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_MORPHA_BATTLE);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_BONGO_BONGO_BATTLE);
        SET_EVENTCHKINF(EVENTCHKINF_BEGAN_TWINROVA_BATTLE);
        gSaveContext.save.info.playerData.savedSceneId = SCENE_TEMPLE_OF_TIME;
        gSaveContext.save.entranceIndex = ENTR_TEMPLE_OF_TIME_0;
        gSaveContext.fileNum = 1; /* file 2 */
        Sram_WriteSave(&sramCtx);
        PortSram_FlushNow();
        gSaveContext.fileNum = 0xFF; /* (no saving from this boot) */
        PortDbg("[fullsave] file 2 written");
    }
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

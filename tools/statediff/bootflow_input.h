/* bootflow_input.h - scripted input + captures for the boot flow (tools/statediff/bootflow.py).
 * Included VERBATIM by both sides in game.c (3DS: STATEDIFF_BOOTFLOW builds; N64 reference: make_ref.sh
 * bootflow mode) and called at the start of GameState_Update, so it covers every game state (console
 * logo, title screen, file select, play). Frames are GameState_Update calls since boot. Controller 1's
 * input is replaced by the script (bootflow_script.h: sBootFlowScript[] { frame, buttons, stickX, stickY },
 * each entry holding until the next) with press/release edges computed like PadMgr; at each frame in
 * sBootFlowCaptures[] the state is captured (N64: breakpoint anchor BootFlow_Captured; 3DS: SD dump). */
#include "libu64/pad.h"
#include "save.h"
#include "bootflow_script.h"

s32 gBootFlowFrame = 0;
s32 gBootFlowCaptureIdx = 0;
static OSContPad sBootFlowPrevPad;

/* N64 breakpoint anchor, a0 = the current game state; runs after the data-cache writeback */
void BootFlow_Captured(GameState* gameState) {
}

void osWritebackDCacheAll(void);

static void BootFlow_Inject(GameState* gameState) {
    Input* input = &gameState->input[0];
    s32 i;
    s32 cur = -1;
    s32 buttonDiff;

    for (i = 0; i < (s32)(sizeof(sBootFlowScript) / sizeof(sBootFlowScript[0])); i++) {
        if (sBootFlowScript[i][0] <= gBootFlowFrame) {
            cur = i;
        }
    }
    input->prev = sBootFlowPrevPad;
    input->cur.button = (cur >= 0) ? (u16)sBootFlowScript[cur][1] : 0;
    input->cur.stick_x = (cur >= 0) ? (s8)sBootFlowScript[cur][2] : 0;
    input->cur.stick_y = (cur >= 0) ? (s8)sBootFlowScript[cur][3] : 0;
    input->cur.errno = 0;
    buttonDiff = input->prev.button ^ input->cur.button;
    input->press.button = input->cur.button & buttonDiff;
    input->rel.button = input->prev.button & buttonDiff;
    input->press.stick_x = 0;
    input->press.stick_y = 0;
    PadUtils_UpdateRelXY(input);
    input->press.stick_x += (s8)(input->cur.stick_x - input->prev.stick_x);
    input->press.stick_y += (s8)(input->cur.stick_y - input->prev.stick_y);
    sBootFlowPrevPad = input->cur;
#ifdef __3DS__
    {
        extern void Port3ds_RequestColor(void);
        Port3ds_RequestColor(); /* keep the last two frames' color readbacks for the dump */
    }
#endif
    if ((gBootFlowCaptureIdx < BOOTFLOW_NCAP) && (gBootFlowFrame == sBootFlowCaptures[gBootFlowCaptureIdx])) {
        osWritebackDCacheAll();
        BootFlow_Captured(gameState);
#ifdef __3DS__
        {
            extern void PortStateDump_Begin(s32 index, const void* play, unsigned playSize, const void* save,
                                            unsigned saveSize);
            extern void PortStateDump_End(void);
            extern void PortStateDump_Blob(const char* name, const void* data, unsigned size);
            extern MtxF gSkinLimbMatrices[60];
            PortStateDump_Begin(gBootFlowCaptureIdx, gameState, sizeof(GameState), &gSaveContext, sizeof(SaveContext));
            PortStateDump_Blob("skin", gSkinLimbMatrices, sizeof(gSkinLimbMatrices));
            PortStateDump_End();
        }
#endif
        gBootFlowCaptureIdx++;
    }
    gBootFlowFrame++;
}

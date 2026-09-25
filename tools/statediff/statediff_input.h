/* statediff_input.h - scripted controller input for tools/statediff comparison builds.
 * Included VERBATIM by both sides: the 3DS port's z_play.c (PORT_STATEDUMP builds) and the N64
 * reference ROM (tools/statediff/make_ref.sh copies it next to upstream z_play.c). At the start of
 * Play_Update, controller 1's input is replaced by the script's value for this gameplay frame and the
 * press/release edges are computed exactly like PadMgr_RequestPadData's non-game-request path.
 * The script (input_script.h, generated from tools/statediff/scripts/<name>.txt by statediff.py)
 * is a list of { firstFrame, buttons, stickX, stickY }; each entry holds until the next one. */
#include "libu64/pad.h"
#include "input_script.h"

static OSContPad sStateDiffPrevPad;

void osWritebackDCacheAll(void);

/* Breakpoint anchor for the N64 capture (tools/statediff sets its breakpoint here, a0 = play). It runs
 * after osWritebackDCacheAll: ares' debugger reads RDRAM, not the CPU's write-back data cache, so
 * without the flush recently written fields read back as stale RAM. No-op on the 3DS. */
void StateDiff_Sync(PlayState* play) {
}

static void StateDiff_InjectInput(PlayState* play) {
    Input* input = &play->state.input[0];
    s32 i;
    s32 cur = -1;
    s32 buttonDiff;

    for (i = 0; i < (s32)(sizeof(sStateDiffScript) / sizeof(sStateDiffScript[0])); i++) {
        if (sStateDiffScript[i][0] <= (s32)play->gameplayFrames) {
            cur = i;
        }
    }
    input->prev = sStateDiffPrevPad;
    input->cur.button = (cur >= 0) ? (u16)sStateDiffScript[cur][1] : 0;
    input->cur.stick_x = (cur >= 0) ? (s8)sStateDiffScript[cur][2] : 0;
    input->cur.stick_y = (cur >= 0) ? (s8)sStateDiffScript[cur][3] : 0;
    input->cur.errno = 0;
    buttonDiff = input->prev.button ^ input->cur.button;
    input->press.button = input->cur.button & buttonDiff;
    input->rel.button = input->prev.button & buttonDiff;
    input->press.stick_x = 0;
    input->press.stick_y = 0;
    PadUtils_UpdateRelXY(input);
    input->press.stick_x += (s8)(input->cur.stick_x - input->prev.stick_x);
    input->press.stick_y += (s8)(input->cur.stick_y - input->prev.stick_y);
    sStateDiffPrevPad = input->cur;
    osWritebackDCacheAll();
    StateDiff_Sync(play);
}

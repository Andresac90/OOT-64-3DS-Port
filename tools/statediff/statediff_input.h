/* statediff_input.h - scripted controller input for tools/statediff comparison builds.
 * Included VERBATIM by both sides: the 3DS port's z_play.c (PORT_STATEDUMP builds) and the N64
 * reference ROM (tools/statediff/make_ref.sh copies it next to upstream z_play.c). At the start of
 * Play_Update, controller 1's input is replaced by the script's value for this gameplay frame and the
 * press/release edges are computed exactly like PadMgr_RequestPadData's non-game-request path.
 * The script (input_script.h, generated from tools/statediff/scripts/<name>.txt by statediff.py)
 * is a list of { firstFrame, buttons, stickX, stickY }; each entry holds until the next one. */
#include "libu64/pad.h"
#include "transition.h"
#include "input_script.h"
#include "tour_script.h" /* sStateDiffTour[] entrances, STATEDIFF_TOUR_LEN, STATEDIFF_TOUR_FRAMES */

/* Scene tour: after STATEDIFF_TOUR_FRAMES frames in each scene, capture (anchor StateDiff_Captured for
 * the N64 side, SD-card dump on the 3DS) and jump to the next entrance with an instant transition. */
s32 gStateDiffCaptureIdx = 0;
/* Watchdog: gameplay frames since the last capture, across scene reloads (some entrances never settle
 * into normal play when entered cold, e.g. re-transitioning every frame). After
 * STATEDIFF_TOUR_FRAMES + 300 frames the capture is forced (same rule on both sides) and recorded in
 * gStateDiffForcedMask so the report can flag it. */
s32 gStateDiffFramesSinceCapture = 0;
u32 gStateDiffForcedMask[4] = { 0, 0, 0, 0 };

void StateDiff_Captured(PlayState* play) {
}

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

    gStateDiffFramesSinceCapture++;
    if (STATEDIFF_TOUR_LEN > 0) {
        // Scene tour: mark Navi's hot-room / underwater warnings as already shown (both sides). Drawing
        // their NAME code with the debug save's name makes the N64 reference fault (FP exception in
        // Message_DrawText, e.g. Death Mountain Crater, Volvagia) - a debug-save artifact.
        gSaveContext.envHazardTextTriggerFlags |= ENV_HAZARD_TEXT_TRIGGER_HOTROOM | ENV_HAZARD_TEXT_TRIGGER_UNDERWATER;
    }
    if ((STATEDIFF_TOUR_LEN > 0) && (gStateDiffCaptureIdx < STATEDIFF_TOUR_LEN) &&
        ((((s32)play->gameplayFrames == STATEDIFF_TOUR_FRAMES) && (play->transitionTrigger == TRANS_TRIGGER_OFF)) ||
         (gStateDiffFramesSinceCapture >= STATEDIFF_TOUR_FRAMES + 300))) {
        if (gStateDiffFramesSinceCapture >= STATEDIFF_TOUR_FRAMES + 300) {
            gStateDiffForcedMask[gStateDiffCaptureIdx >> 5] |= 1u << (gStateDiffCaptureIdx & 31);
        }
        gStateDiffFramesSinceCapture = 0;
        StateDiff_Captured(play);
#ifdef __3DS__
        StateDiff_PortDump(play, gStateDiffCaptureIdx);
#endif
        gStateDiffCaptureIdx++;
        if (gStateDiffCaptureIdx < STATEDIFF_TOUR_LEN) {
            // leave any cutscene behind, like a normal exit: a scripted-cutscene index carried into a
            // scene without that cutscene makes the real N64 read a NULL script (Cutscene_ProcessScript
            // MemCpy fault, seen after Ganon's intro in the child tour)
            gSaveContext.save.cutsceneIndex = 0;
            gSaveContext.cutsceneTrigger = 0;
            play->nextEntranceIndex = sStateDiffTour[gStateDiffCaptureIdx];
            play->transitionTrigger = TRANS_TRIGGER_START;
            play->transitionType = TRANS_TYPE_INSTANT;
        }
    }
}

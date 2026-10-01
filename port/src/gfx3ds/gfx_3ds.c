#ifdef TARGET_N3DS

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <citro3d.h>
#include "gfx_3ds.h"

// #define DISPLAY_TRANSFER_FLAGS_NORMAL \
// 	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
// 	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
// 	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NONE))

// #define DISPLAY_TRANSFER_FLAGS_AA_12 \
// 	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
// 	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
// 	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_X))

// #define DISPLAY_TRANSFER_FLAGS_AA_22 \
// 	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
// 	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
// 	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY))

// #ifdef N3DS_USE_ANTIALIASING

// #define DISPLAY_TRANSFER_FLAGS \
// 	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
// 	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
// 	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY))

// #else

// #define DISPLAY_TRANSFER_FLAGS \
// 	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
// 	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
// 	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NONE))

// #endif

static C3D_RenderTarget* sTarget;
/* PORT (2026-09-30): stereoscopic 3D. With the 3D slider up the top screen switches from the mono mode
 * (800px wide + AA on hardware; the wide mode cannot show 3D) to two 400x240 eyes rendered side by side
 * into one 240x800 target: every draw is issued twice with a different viewport and eye uniform
 * (gfx_citro3d.c), so the display list is processed once and no render-target switching is needed. Two
 * output-only targets alias the halves, so citro3d's FrameEnd transfers them to the left/right
 * framebuffers itself. Slider down: back to the mono mode, zero extra cost. */
float gPortStereoSep;
/* convergence (clip w at screen depth) for the stereo shader: Link's distance from the camera, set by
 * the game each frame (gPortStereoFocusW, z_play.c; 0 when there is no player), smoothed so camera
 * cuts and Link's movement don't make the depth jump. */
float gPortStereoFocusW;
int gPortStereoFlatScene; /* set by menu gamestates (file select) each frame; cleared after the frame */
float gPortStereoConv = 150.0f;
static Gfx3DSMode sMonoMode;
static u32 sMonoFlags;
static int sMonoW, sMonoH;
static bool sMonoWide;
static C3D_RenderTarget* sEyeOut[2];

/* the region readbacks use: one eye in stereo (the left half, laid out like the normal mode) */
static int ViewW(void) {
    return sTarget->frameBuf.width;
}
static int ViewH(void) {
    return gGfx3DSMode == GFX_3DS_MODE_STEREO ? STEREO_EYE_OFFSET : sTarget->frameBuf.height;
}

Gfx3DSMode gGfx3DSMode;

static bool checkN3DS()
{
    bool isNew3DS = false;

	if (R_SUCCEEDED(APT_CheckNew3DS(&isNew3DS)))
		return isNew3DS;

	return false;
}

static void gfx_3ds_init(void) 
{
    extern int gPortO3dsSim;
    if (checkN3DS() && !gPortO3dsSim)
		osSetSpeedupEnable(true);

    /* PORT (2026-09-30): gfx is already initialized by main() (for the boot log). A second gfxInitDefault
     * took a second GSP reference (libctru reference-counts gspInit) and allocated unused framebuffers;
     * the single gfxExit at shutdown then left GSP's event thread running while exit() unmapped the heap
     * holding its stack: HOME -> Close crashed in gspEventThreadMain (hardware v17, v18). */
    consoleInit(GFX_BOTTOM, NULL);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    bool useAA = false;
#ifdef N3DS_USE_ANTIALIASING
    useAA = true;
#endif
    bool useWide = false;
#ifdef N3DS_USE_WIDE_800PX
    u8 model;
    CFGU_GetSystemModel(&model);
    useWide = model != 3; //wide is not possible on o2ds
    /* PORT (2026-09-24): Citra-family emulators (Azahar) don't implement the 800px wide
     * top-screen mode -- they show only the left 400 columns at 2x, which looked like an
     * off-center camera and pushed centered UI (the pause menu) off-screen. Detect the
     * emulator via its emulator-only system-info type (0x20000); real hardware returns an
     * error there, so hardware keeps the sharper wide mode. */
    {
        s64 emu = 0;
        if (R_SUCCEEDED(svcGetSystemInfo(&emu, 0x20000, 0)))
            useWide = false;
    }
#endif

    u32 transferFlags = 
        GX_TRANSFER_FLIP_VERT(0) | 
        GX_TRANSFER_OUT_TILED(0) | 
        GX_TRANSFER_RAW_COPY(0) |
	    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | 
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8);

    if (useAA && !useWide)
        transferFlags |= GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY);
    else if (useAA && useWide)
        transferFlags |= GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_X);
    else
        transferFlags |= GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

    int width = useAA || useWide ? 800 : 400;
    int height = useAA ? 480 : 240;

    sTarget = C3D_RenderTargetCreate(height, width, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT, transferFlags);
    sMonoFlags = transferFlags;
    sMonoW = height;
    sMonoH = width;
    sMonoWide = useWide;

    if (!useAA && !useWide)
        gGfx3DSMode = GFX_3DS_MODE_NORMAL;
    else if (useAA && !useWide)
        gGfx3DSMode = GFX_3DS_MODE_AA_22;
    else if (!useAA && useWide)
        gGfx3DSMode = GFX_3DS_MODE_WIDE;
    else
        gGfx3DSMode = GFX_3DS_MODE_WIDE_AA_12;

    if (useWide)
        gfxSetWide(true);
    sMonoMode = gGfx3DSMode;
}

/* Called before C3D_FrameBegin: citro3d breaks (svcBreak) on C3D_RenderTargetDelete inside a frame, and
 * the delete waits for the GPU queue itself. The mono and stereo targets never coexist (VRAM: the
 * 480x800 AA target alone is 3 MB). */
static void gfx_3ds_update_stereo(void) {
    float slider = osGet3DSliderState();
    bool want = slider > 0.02f;
    bool on = gGfx3DSMode == GFX_3DS_MODE_STEREO;
    int e;

    /* full slider: 0.04 NDC per eye = ~16 px of disparity at infinity (v1 had 0.03 / 12 px and a
     * fixed convergence: "not deep enough" on hardware) */
    gPortStereoSep = want ? slider * 0.04f : 0.0f;
    {
        float target = gPortStereoFocusW;
        float border = 0.0f;
        { /* the nearest 3D surface at the screen borders (gfx_pc.c stereo_probe_tri): the 3rd-nearest
           * of the 13 border probes, so one particle near the lens doesn't drag the screen plane */
            extern float gPortStereoProbeW[13];
            float v[13];
            int n = 0, a, b;
            for (a = 0; a < 13; a++) {
                if (gPortStereoProbeW[a] > 0.0f) v[n++] = gPortStereoProbeW[a];
                gPortStereoProbeW[a] = 0.0f;
            }
            for (a = 1; a < n; a++) { /* insertion sort */
                float x = v[a];
                for (b = a - 1; b >= 0 && v[b] > x; b--) v[b + 1] = v[b];
                v[b + 1] = x;
            }
            if (n >= 3) border = v[2];
            else if (n > 0) border = v[n - 1];
        }
        if (border > 0.0f && border < target) target = border; /* keep border geometry out of the air */
        if (target < 10.0f) target = 10.0f;  /* the camera skimming the ground (title intro: w = 16) */
        if (target > 600.0f) target = 600.0f;
        {
            /* pre-rendered rooms (a S2DEX background was drawn last frame): converge at half Link's
             * distance so he sits ~50% deep, inside the room picture (drawn at 75%) */
            extern int gPortPrerenderedFrame;
            if (gPortPrerenderedFrame) target *= 0.5f;
        }
        /* asymmetric: pull the screen plane nearer fast (border geometry must never pop out), let it
         * recede slowly (no depth "breathing" when the border estimate jumps between frames) */
        gPortStereoConv += (target - gPortStereoConv) * (target < gPortStereoConv ? 0.3f : 0.04f);
    }
    if (want == on) {
        return;
    }
    { extern void PortDbg(const char*); PortDbg(want ? "[stereo] enter" : "[stereo] leave"); }
    if (want) {
        C3D_RenderTarget* st;
        static int sRetryWait;
        if (sRetryWait > 0) { /* a failed switch retries once a second, not every frame */
            sRetryWait--;
            gPortStereoSep = 0.0f;
            return;
        }
        /* free the mono target FIRST: VRAM cannot hold both (plus the pause menu's off-screen targets,
         * which stay allocated once used; creating the stereo target next to the mono one failed on
         * hardware after a pause, and 3D never came back) */
        C3D_RenderTargetDelete(sTarget); /* also unlinks it from the top screen */
        sTarget = NULL;
        st = C3D_RenderTargetCreate(240, 2 * STEREO_EYE_OFFSET, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        for (e = 0; e < 2 && st != NULL; e++) {
            sEyeOut[e] = C3D_RenderTargetCreate(240, STEREO_EYE_OFFSET, GPU_RB_RGBA8, -1);
            if (sEyeOut[e] != NULL) {
                /* alias half e of the stereo target (the tiled buffer is contiguous along the long axis) */
                vramFree(sEyeOut[e]->frameBuf.colorBuf);
                sEyeOut[e]->frameBuf.colorBuf = (u8*)st->frameBuf.colorBuf + e * 240 * STEREO_EYE_OFFSET * 4;
                sEyeOut[e]->ownsColor = false;
            }
        }
        if (st == NULL || sEyeOut[0] == NULL || sEyeOut[1] == NULL) {
            { extern void PortDbg(const char*); PortDbg("[stereo] no VRAM for the stereo target, staying mono"); }
            for (e = 0; e < 2; e++) {
                if (sEyeOut[e] != NULL) C3D_RenderTargetDelete(sEyeOut[e]), sEyeOut[e] = NULL;
            }
            if (st != NULL) C3D_RenderTargetDelete(st);
            sTarget = C3D_RenderTargetCreate(sMonoW, sMonoH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT, sMonoFlags);
            gPortStereoSep = 0.0f;
            sRetryWait = 60;
            return;
        }
        sTarget = st;
        {
            u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
            C3D_RenderTargetSetOutput(sEyeOut[0], GFX_TOP, GFX_LEFT, flags);
            C3D_RenderTargetSetOutput(sEyeOut[1], GFX_TOP, GFX_RIGHT, flags);
        }
        gfxSetWide(false);
        gfxSet3D(true);
        gGfx3DSMode = GFX_3DS_MODE_STEREO;
    } else {
        for (e = 0; e < 2; e++) {
            C3D_RenderTargetDelete(sEyeOut[e]); /* not the owner of the color buffer */
            sEyeOut[e] = NULL;
        }
        C3D_RenderTargetDelete(sTarget); /* first, same VRAM reason as above */
        sTarget = C3D_RenderTargetCreate(sMonoW, sMonoH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT, sMonoFlags);
        gfxSet3D(false);
        gfxSetWide(sMonoWide);
        gGfx3DSMode = sMonoMode;
    }
    /* the depth/color readbacks were sized for the old target */
    {
        extern void gfx_3ds_drop_readbacks(void);
        gfx_3ds_drop_readbacks();
    }
}

static void gfx_3ds_main_loop(void (*run_one_game_iter)(void)) 
{
    while (aptMainLoop())
        run_one_game_iter();

    ndspExit();
    C3D_Fini();
	gfxExit();
}

static void gfx_3ds_get_dimensions(uint32_t *width, uint32_t *height) 
{
    *width = 400;
    *height = 240;
}

/* PORT (2026-09-29): service the HOME button / sleep / power once per frame (the game's own loop never
 * reaches gfx_3ds_main_loop, so nothing called aptMainLoop and HOME did nothing). aptMainLoop blocks
 * while the HOME menu is open and returns false when the user closes the software. */
static void gfx_3ds_handle_events(void)
{
    if (!aptMainLoop()) {
        /* PORT (2026-09-30): shut graphics down before exit(). exit() unmaps the app heap, and libctru's
         * GSP event thread (stack on that heap) was still running: HOME -> Close crashed with a data
         * abort in gspEventThreadMain (hardware, v17). gfxExit stops that thread. */
        ndspExit();
        C3D_Fini();
        gfxExit();
        exit(0);
    }
}

/* Depth readback for the game's CPU reads of the N64 z-buffer (point-light glows in z_lights.c, the
 * sun's lens-flare test in z_kankyo.c). When a frame's render completes, its D24S8 depth buffer is copied (GPU display transfer, detiled) into linear memory; the copy
 * only runs while the game has asked for depth recently. port/src/zbuffer_port.c converts samples to
 * N64 z-buffer words. */
static u32* sDepthLinear[2]; /* same slot convention as the color readback below */
static int sDepthSlot = -1;   /* slot of the most recent depth readback */
static int sDepthWantFrames;
static bool sDepthValid;
static int sFrameSlot;        /* slot this frame's readbacks go to (alternates each frame) */

void Port3ds_RequestDepth(void) {
    sDepthWantFrames = 60;
}

const u32* Port3ds_GetDepth(int* width, int* height) {
    *width = ViewW();
    *height = ViewH();
    return (sDepthValid && sDepthSlot >= 0) ? sDepthLinear[sDepthSlot] : NULL;
}

/* back = 0: most recent frame, 1: the one before (tools/statediff dumps both with the colors) */
const u32* Port3ds_GetDepthSlot(int back, int* width, int* height) {
    *width = ViewW();
    *height = ViewH();
    if (sDepthSlot < 0) {
        return NULL;
    }
    return sDepthLinear[back ? (sDepthSlot ^ 1) : sDepthSlot];
}

static void gfx_3ds_read_back_depth(void) {
    size_t size = (size_t)ViewW() * ViewH() * 4;

    if (sDepthWantFrames <= 0) {
        sDepthValid = false;
        return;
    }
    sDepthWantFrames--;
    if (sDepthLinear[sFrameSlot] == NULL) {
        sDepthLinear[sFrameSlot] = linearAlloc(size);
        if (sDepthLinear[sFrameSlot] == NULL) {
            return;
        }
    }
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.depthBuf,
                            GX_BUFFER_DIM(ViewW(), ViewH()), sDepthLinear[sFrameSlot],
                            GX_BUFFER_DIM(ViewW(), ViewH()),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(sDepthLinear[sFrameSlot], size);
    sDepthSlot = sFrameSlot;
    sDepthValid = true;
}

/* Color readback for tools/statediff (renderer ground truth vs ares' RDP framebuffer): the last two
 * finished frames, detiled into linear RGBA8, kept alternately. Only runs while a comparison build asks
 * for it (Port3ds_RequestColor), so normal play pays nothing. */
static u32* sColorLinear[2];
static int sColorWantFrames;
static int sColorLatest = -1; /* slot holding the most recently finished frame */

void Port3ds_RequestColor(void) {
    sColorWantFrames = 60;
}

int Port3ds_DrawIdActive(void) {
    return sColorWantFrames > 0;
}

int Port3ds_ColorLatestSlot(void) {
    return sColorLatest;
}

/* back = 0: most recently finished frame; back = 1: the one before */
const u32* Port3ds_GetColor(int back, int* width, int* height) {
    int slot;
    *width = ViewW();
    *height = ViewH();
    if (sColorLatest < 0) {
        return NULL;
    }
    slot = back ? (sColorLatest ^ 1) : sColorLatest;
    return sColorLinear[slot];
}

static void gfx_3ds_read_back_color(void) {
    size_t size = (size_t)ViewW() * ViewH() * 4;
    int slot;

    if (sColorWantFrames <= 0) {
        return;
    }
    sColorWantFrames--;
    slot = sFrameSlot;
    if (sColorLinear[slot] == NULL) {
        sColorLinear[slot] = linearAlloc(size);
        if (sColorLinear[slot] == NULL) {
            return;
        }
    }
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.colorBuf,
                            GX_BUFFER_DIM(ViewW(), ViewH()), sColorLinear[slot],
                            GX_BUFFER_DIM(ViewW(), ViewH()),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(sColorLinear[slot], size);
    sColorLatest = slot;
}

/* PORT (2026-09-28): framebuffer capture for PreRender (pause-menu background, transition tiles). On the
 * N64 the RDP copies the rendered frame into fbufSave (gZBuffer memory) and the CPU/RDP read it back
 * later. Here the frame is read back after the GPU finishes it, box-filtered from the 2x2 supersamples,
 * the pillarbox dropped, and written as 320x240 RGBA5551 in the texture memory layout (u64-swizzled:
 * 16-bit pixel k at native index k ^ 3), so the game's own restore draws it like any texture. */
static void* sCaptureDst;

void Port3ds_CaptureFrame5551(void* dst) {
    sCaptureDst = dst;
}

/* 60 fps interpolation: no in-between frames while a capture/readback is pending for this frame */
int Port3ds_InterpBlocked(void) {
    return sCaptureDst != NULL || sColorWantFrames > 0;
}

/* buffers sized for the previous target: dropped on a stereo switch, reallocated at the new size */
void gfx_3ds_drop_readbacks(void) {
    int i;
    for (i = 0; i < 2; i++) {
        if (sDepthLinear[i] != NULL) linearFree(sDepthLinear[i]), sDepthLinear[i] = NULL;
        if (sColorLinear[i] != NULL) linearFree(sColorLinear[i]), sColorLinear[i] = NULL;
    }
    sDepthValid = false;
    sDepthSlot = -1;
    sColorLatest = -1;
}

static void gfx_3ds_capture_frame(void) {
    extern void gfx_texture_cache_invalidate_range(const void* start, uint32_t size);
    static u32* sLin;
    int W = ViewW(), H = ViewH(), sx = H / 400, sy = W / 240, x, y, ox, oy;
    size_t size = (size_t)W * H * 4;
    uint16_t* dst = (uint16_t*)sCaptureDst;

    if (dst == NULL) {
        return;
    }
    sCaptureDst = NULL;
    /* sized for the mono target, the larger of the two (stereo reads one 240x400 eye) */
    if (sLin == NULL && (sLin = linearAlloc((size_t)sMonoW * sMonoH * 4)) == NULL) {
        return;
    }
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.colorBuf, GX_BUFFER_DIM(W, H), sLin, GX_BUFFER_DIM(W, H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(sLin, size);
    for (y = 0; y < 240; y++) {
        for (x = 0; x < 320; x++) {
            unsigned r = 0, g = 0, b = 0, n = (unsigned)(sx * sy);
            for (ox = 0; ox < sx; ox++) {
                for (oy = 0; oy < sy; oy++) {
                    u32 w = sLin[((x + 40) * sx + ox) * W + (W - 1 - (y * sy + oy))]; /* 0xRRGGBBAA */
                    r += (w >> 24) & 0xFF;
                    g += (w >> 16) & 0xFF;
                    b += (w >> 8) & 0xFF;
                }
            }
            r /= n;
            g /= n;
            b /= n;
            dst[(y * 320 + x) ^ 3] = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
        }
    }
    gfx_texture_cache_invalidate_range(dst, 320 * 240 * 2);
}

/* PORT (2026-09-28): off-screen color images (the pause menu's Link preview, Player_DrawPause, and any other
 * gDPSetColorImage to a non-framebuffer address). Draws go to a 1x 320x240-space render target (portrait,
 * like the screen, no pillarbox); after the frame the drawn region (0..width x 0..height, from the
 * scissor) is read back into the game's buffer as RGBA5551 in texture layout, so the game draws or
 * processes it like the N64's RDP output - one frame later than on the N64. */
#define OFFSCREEN_MAX 4
static struct {
    void* addr;
    C3D_RenderTarget* rt;
    int width, height; /* image size in pixels written back */
    int used;          /* drawn this frame */
} sOff[OFFSCREEN_MAX];
static int sOffCur = -1; /* current target, -1 = screen */

int Port3ds_IsOffscreen(void) {
    return sOffCur >= 0;
}

void Port3ds_SetDrawTarget(void* addr, int width, int height) {
    int i, slot = -1;
    if (addr == NULL) {
        if (sOffCur >= 0) {
            C3D_FrameDrawOn(sTarget);
            sOffCur = -1;
        }
        return;
    }
    for (i = 0; i < OFFSCREEN_MAX; i++) {
        if (sOff[i].addr == addr) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (i = 0; i < OFFSCREEN_MAX && slot < 0; i++) {
            if (sOff[i].addr == NULL || !sOff[i].used) {
                slot = i;
            }
        }
        if (slot < 0) {
            return;
        }
        if (sOff[slot].rt == NULL) {
            sOff[slot].rt = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            if (sOff[slot].rt == NULL) {
                return;
            }
        }
        sOff[slot].addr = addr;
        sOff[slot].used = 0;
    }
    if (width > 0) {
        sOff[slot].width = width > 320 ? 320 : width;
    }
    if (height > sOff[slot].height) {
        sOff[slot].height = height > 240 ? 240 : height;
    }
    if (!sOff[slot].used) {
        C3D_RenderTargetClear(sOff[slot].rt, C3D_CLEAR_ALL, 0x000000FF, 0xFFFFFFFF);
        sOff[slot].used = 1;
    }
    C3D_FrameDrawOn(sOff[slot].rt);
    sOffCur = slot;
}

static void gfx_3ds_read_back_offscreen(void) {
    extern void gfx_texture_cache_invalidate_range(const void* start, uint32_t size);
    static u32* sLin;
    int i, x, y;
    const int W = 240, H = 320;
    if (sLin == NULL && (sLin = linearAlloc(W * H * 4)) == NULL) {
        return;
    }
    for (i = 0; i < OFFSCREEN_MAX; i++) {
        uint16_t* dst = (uint16_t*)sOff[i].addr;
        int w = sOff[i].width, h = sOff[i].height;
        if (!sOff[i].used || dst == NULL || w <= 0 || h <= 0) {
            continue;
        }
        sOff[i].used = 0;
        C3D_SyncDisplayTransfer((u32*)sOff[i].rt->frameBuf.colorBuf, GX_BUFFER_DIM(W, H), sLin, GX_BUFFER_DIM(W, H),
                                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
        GSPGPU_InvalidateDataCache(sLin, W * H * 4);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                u32 p = sLin[x * W + (W - 1 - y)]; /* 0xRRGGBBAA */
                dst[(y * w + x) ^ 3] = (uint16_t)(((((p >> 24) & 0xFF) >> 3) << 11) | ((((p >> 16) & 0xFF) >> 3) << 6) |
                                                  ((((p >> 8) & 0xFF) >> 3) << 1) | 1);
            }
        }
        gfx_texture_cache_invalidate_range(dst, (uint32_t)(w * h * 2));
    }
}

/* verification aid: with sdmc:/3ds/oot/capture_stereo present, every 300 frames while in stereo the
 * whole 240x800 target (both eyes, detiled RGBA8) goes to sdmc:/3ds/oot/stereo_fb_<n>.bin (header w, h) */
static void gfx_3ds_debug_dump_stereo(void) {
    static int sFrames;
    static u32* sLin;
    FILE* f;
    int W, H;
    if (gGfx3DSMode != GFX_3DS_MODE_STEREO || (++sFrames % 300) != 0) {
        return;
    }
    f = fopen("sdmc:/3ds/oot/capture_stereo", "rb");
    if (f == NULL) {
        return;
    }
    fclose(f);
    W = sTarget->frameBuf.width, H = sTarget->frameBuf.height;
    if (sLin == NULL && (sLin = linearAlloc((size_t)W * H * 4)) == NULL) {
        return;
    }
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.colorBuf, GX_BUFFER_DIM(W, H), sLin, GX_BUFFER_DIM(W, H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(sLin, (size_t)W * H * 4);
    {
        char path[64];
        snprintf(path, sizeof path, "sdmc:/3ds/oot/stereo_fb_%d.bin", sFrames / 300);
        f = fopen(path, "wb");
    }
    if (f != NULL) {
        u32 hdr[2] = { (u32)W, (u32)H };
        fwrite(hdr, 4, 2, f);
        fwrite(sLin, 4, (size_t)W * H, f);
        fclose(f);
    }
}

/* verification aid for the 60 fps interpolation: with sdmc:/3ds/oot/capture_interp present, every 200th
 * logic frame's passes (in-between ones, then the exact one) go to sdmc:/3ds/oot/interp_fb_<n>_<pass>.bin */
static void gfx_3ds_debug_dump_interp(void) {
    extern int gPortInterpExtra;
    static int sLogic, sPass, sOn = -1;
    static u32* sLin;
    FILE* f;
    int W, H;
    if (sOn < 0) {
        f = fopen("sdmc:/3ds/oot/capture_interp", "rb");
        sOn = f != NULL;
        if (f != NULL) fclose(f);
    }
    if (!sOn) {
        return;
    }
    if ((sLogic % 200) >= 198) {
        W = sTarget->frameBuf.width, H = sTarget->frameBuf.height;
        if (sLin == NULL) sLin = linearAlloc((size_t)W * H * 4 * 2);
        if (sLin != NULL) {
            char path[64];
            C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.colorBuf, GX_BUFFER_DIM(W, H), sLin, GX_BUFFER_DIM(W, H),
                                    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
            GSPGPU_InvalidateDataCache(sLin, (size_t)W * H * 4);
            snprintf(path, sizeof path, "sdmc:/3ds/oot/interp_fb_%d_%d.bin", sLogic, sPass);
            f = fopen(path, "wb");
            if (f != NULL) {
                u32 hdr[2] = { (u32)W, (u32)H };
                fwrite(hdr, 4, 2, f);
                fwrite(sLin, 4, (size_t)W * H, f);
                fclose(f);
            }
        }
    }
    sPass++;
    if (!gPortInterpExtra) {
        sLogic++;
        sPass = 0;
    }
}

u64 gPortPerfGpuWait; /* ticks in C3D_FrameBegin: waiting for the previous frame's GPU work */

static bool gfx_3ds_start_frame(void)
{
    u64 t0;
    gfx_3ds_update_stereo(); /* outside a frame: citro3d refuses to delete targets inside one */
    t0 = svcGetSystemTick();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    gPortPerfGpuWait += svcGetSystemTick() - t0;
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0xFFFFFFFF);
	C3D_FrameDrawOn(sTarget);
    return true;
}

static void gfx_3ds_swap_buffers_begin(void) 
{
    if (gGfx3DSMode == GFX_3DS_MODE_STEREO) {
        /* drawn through the stereo target: mark the aliased outputs for FrameEnd's transfers */
        sEyeOut[0]->used = sEyeOut[1]->used = true;
    }
    {
        extern void gfx_citro3d_flush_vbo(void);
        gfx_citro3d_flush_vbo(); /* CPU-written vertices must reach RAM before the GPU runs the frame */
    }
    C3D_FrameEnd(0);
    {
        /* an in-between frame (60 fps interpolation) re-draws the same display list: the logic frame's
         * readbacks and captures belong to its exact pass, which comes after it */
        extern int gPortInterpExtra;
        if (gPortInterpExtra) {
            gfx_3ds_debug_dump_interp();
            sOffCur = -1;
            return;
        }
        gfx_3ds_debug_dump_interp();
    }
    gPortStereoFlatScene = 0; /* the next frame's gamestate sets it again if it is a menu */
    /* Depth readback right after this frame's render (C3D_SyncDisplayTransfer outside a frame waits
     * for the queued render first). The game samples it from Environment_GraphCallback, which the N64
     * runs once the previous frame's RDP work is done: reading it back here gives the same frame N-1
     * depth. (Reading at the next start_frame was one frame older - measured with tools/statediff:
     * Navi's glow in the adult Water Temple flipped.) */
    gfx_3ds_read_back_depth();
    gfx_3ds_read_back_color();
    gfx_3ds_capture_frame();
    gfx_3ds_read_back_offscreen();
    gfx_3ds_debug_dump_stereo();
    sOffCur = -1;
    sFrameSlot ^= 1; /* next frame's readbacks go to the other slot */
    /* PORT (2026-09-24): no vblank wait here -- Port3ds_PaceFrame (3ds_main.c) paces updates to the
     * game's R_UPDATE_RATE retraces and pumps audio per retrace. */
}

static void gfx_3ds_swap_buffers_end(void) 
{
}

static double gfx_3ds_get_time(void) 
{
    return 0.0;
}

struct GfxWindowManagerAPI gfx_3ds =
{
    gfx_3ds_init,
    gfx_3ds_main_loop,
    gfx_3ds_get_dimensions,
    gfx_3ds_handle_events,
    gfx_3ds_start_frame,
    gfx_3ds_swap_buffers_begin,
    gfx_3ds_swap_buffers_end,
    gfx_3ds_get_time
};

#endif
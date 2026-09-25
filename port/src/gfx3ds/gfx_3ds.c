#ifdef TARGET_N3DS

#include <3ds.h>
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
    if (checkN3DS())
		osSetSpeedupEnable(true);

    gfxInitDefault();
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

static void gfx_3ds_handle_events(void) 
{
    
}

/* Depth readback for the game's CPU reads of the N64 z-buffer (point-light glows in z_lights.c, the
 * sun's lens-flare test in z_kankyo.c). When a frame's render completes, its D24S8 depth buffer is copied (GPU display transfer, detiled) into linear memory; the copy
 * only runs while the game has asked for depth recently. port/src/zbuffer_port.c converts samples to
 * N64 z-buffer words. */
static u32* sDepthLinear;
static int sDepthWantFrames;
static bool sDepthValid;

void Port3ds_RequestDepth(void) {
    sDepthWantFrames = 60;
}

const u32* Port3ds_GetDepth(int* width, int* height) {
    *width = sTarget->frameBuf.width;
    *height = sTarget->frameBuf.height;
    return sDepthValid ? sDepthLinear : NULL;
}

static void gfx_3ds_read_back_depth(void) {
    size_t size = (size_t)sTarget->frameBuf.width * sTarget->frameBuf.height * 4;

    if (sDepthWantFrames <= 0) {
        sDepthValid = false;
        return;
    }
    sDepthWantFrames--;
    if (sDepthLinear == NULL) {
        sDepthLinear = linearAlloc(size);
        if (sDepthLinear == NULL) {
            return;
        }
    }
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.depthBuf,
                            GX_BUFFER_DIM(sTarget->frameBuf.width, sTarget->frameBuf.height), sDepthLinear,
                            GX_BUFFER_DIM(sTarget->frameBuf.width, sTarget->frameBuf.height),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(sDepthLinear, size);
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

/* back = 0: most recently finished frame; back = 1: the one before */
const u32* Port3ds_GetColor(int back, int* width, int* height) {
    int slot;
    *width = sTarget->frameBuf.width;
    *height = sTarget->frameBuf.height;
    if (sColorLatest < 0) {
        return NULL;
    }
    slot = back ? (sColorLatest ^ 1) : sColorLatest;
    return sColorLinear[slot];
}

static void gfx_3ds_read_back_color(void) {
    size_t size = (size_t)sTarget->frameBuf.width * sTarget->frameBuf.height * 4;
    int slot;

    if (sColorWantFrames <= 0) {
        return;
    }
    sColorWantFrames--;
    slot = (sColorLatest < 0) ? 0 : (sColorLatest ^ 1);
    if (sColorLinear[slot] == NULL) {
        sColorLinear[slot] = linearAlloc(size);
        if (sColorLinear[slot] == NULL) {
            return;
        }
    }
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.colorBuf,
                            GX_BUFFER_DIM(sTarget->frameBuf.width, sTarget->frameBuf.height), sColorLinear[slot],
                            GX_BUFFER_DIM(sTarget->frameBuf.width, sTarget->frameBuf.height),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(sColorLinear[slot], size);
    sColorLatest = slot;
}

static bool gfx_3ds_start_frame(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0xFFFFFFFF);
	C3D_FrameDrawOn(sTarget);
    return true;
}

static void gfx_3ds_swap_buffers_begin(void) 
{
    C3D_FrameEnd(0);
    /* Depth readback right after this frame's render (C3D_SyncDisplayTransfer outside a frame waits
     * for the queued render first). The game samples it from Environment_GraphCallback, which the N64
     * runs once the previous frame's RDP work is done: reading it back here gives the same frame N-1
     * depth. (Reading at the next start_frame was one frame older - measured with tools/statediff:
     * Navi's glow in the adult Water Temple flipped.) */
    gfx_3ds_read_back_depth();
    gfx_3ds_read_back_color();
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
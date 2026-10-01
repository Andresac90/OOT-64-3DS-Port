#include <malloc.h>
/*
 * 3ds_main.c — Nintendo 3DS entry point (libctru). Replaces pc_main.c/pc_gfx.c.
 * Boots the port runtime, drives the OoT gamestate loop, and reads the real
 * 3DS buttons into the controller shim. Rendering goes through the citro3d
 * backend (gfx3ds/gfx_citro3d.c) via the same gfx_pc interface proven on PC.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <math.h>

#include <PR/gbi.h>
#include "ultra64/sptask.h"
#include "gfx_pc.h"
#include "gfx_3ds.h"
#include "gfx_rendering_api.h"

/* N64 controller button bits (from include/controller.h) */
#define BTN_A_      0x8000
#define BTN_B_      0x4000
#define BTN_Z_      0x2000
#define BTN_START_  0x1000
#define BTN_DUP_    0x0800
#define BTN_DDOWN_  0x0400
#define BTN_DLEFT_  0x0200
#define BTN_DRIGHT_ 0x0100
#define BTN_L_      0x0020
#define BTN_R_      0x0010
#define BTN_CUP_    0x0008
#define BTN_CDOWN_  0x0004
#define BTN_CLEFT_  0x0002
#define BTN_CRIGHT_ 0x0001

extern struct GfxWindowManagerAPI gfx_3ds;
extern struct GfxRenderingAPI gfx_citro3d_api;

extern void Main(void* arg);
extern void Graph_ThreadEntry(void* arg);
extern void PortDma_Init(const char* romPath);

/* VI config globals the boot path expects (idle.c) */
extern unsigned char gViConfigModeType;
extern void* osViModeNtscLan1;

/* read by osContGetReadData via the shim */
static unsigned short s3dsButtons;
static signed char s3dsStickX, s3dsStickY;

unsigned short PortInput_GetPad(signed char* outX, signed char* outY) {
    if (outX) *outX = s3dsStickX;
    if (outY) *outY = s3dsStickY;
    return s3dsButtons;
}

/* PORT (2026-09-30): widescreen option (gfx_pc.c gPortWidescreen): SELECT toggles it (the N64 pad has
 * no SELECT), saved in sdmc:/3ds/oot/settings.txt. Off = the N64's 4:3 picture with side bars. */
#define PORT_SETTINGS_PATH "sdmc:/3ds/oot/settings.txt"
static int sO3dsSimSetting;
/* bench=1: A/B benchmark - frames alternate between the indexed and the array vertex path (gfx_pc.c
 * gPortLegacyVbo); display-list time is accumulated per variant and logged with each report. Leave the
 * title screen's attract demo running (same content every boot). */
static int sBench;
/* prof=1: sampling profiler (port_prof.h) */
#include "port_prof.h"
volatile unsigned char gPortProf;
static int sProfOn;
int gPortStereoTest; /* settings stereo_test=1: 3D toggles every 2 s (gfx_3ds.c), freeze reproduction */
static volatile u32 sProfHist[PROF_COUNT];
static volatile int sProfRun;
static void Port3ds_ProfThread(void* arg) {
    (void)arg;
    while (sProfRun) {
        svcSleepThread(250000); /* 250 us */
        if (gPortProf < PROF_COUNT) sProfHist[gPortProf]++;
    }
}
static void Port3ds_ProfStart(void) {
    s32 prio = 0x30;
    if (!sProfOn) return;
    sProfRun = 1;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* another core (the main thread is on core 0): 1 = the system core on Old 3DS, also present on New */
    if (threadCreate(Port3ds_ProfThread, NULL, 4096, prio - 2, 1, true) == NULL) {
        threadCreate(Port3ds_ProfThread, NULL, 4096, prio - 2, -2, true);
    }
}
/* fps60=0 turns the 60 fps frame interpolation off (on by default; docs/3ds-60fps-plan.md) */
static int sInterp = 1;
static u64 sBenchDl[2];
static u32 sBenchFrames[2]; /* o3ds_sim as read from settings.txt (not perf_ab's run-time toggling) */
static void Port3ds_SaveSettings(void) {
    extern int gPortWidescreen;
    FILE* f = fopen(PORT_SETTINGS_PATH, "w");
    if (f != NULL) {
        extern int gPortHudTop;
        fprintf(f, "widescreen=%d\n", gPortWidescreen ? 1 : 0);
        fprintf(f, "hud=%d\n", gPortHudTop ? 1 : 0);
        {
            /* measurement switches are written back as they were read (perf_ab toggles gPortO3dsSim at
             * run time; saving that state made the next boot start in Old-3DS speed) */
            extern int gPortPerfStagesOn, gPortPerfAB;
            if (sO3dsSimSetting) fprintf(f, "o3ds_sim=1\n");
            if (gPortPerfStagesOn) fprintf(f, "perf_stages=1\n");
            if (gPortPerfAB) fprintf(f, "perf_ab=1\n");
            if (sProfOn) fprintf(f, "prof=1\n");
            { extern int gPortGpuVtx; if (gPortGpuVtx) fprintf(f, "gpu_vtx=1\n"); }
            if (sInterp != 1) fprintf(f, "fps60=%d\n", sInterp);
        }
        fclose(f);
    }
}
static void Port3ds_LoadSettings(void) {
    extern int gPortWidescreen;
    char line[64];
    FILE* f = fopen(PORT_SETTINGS_PATH, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        int v;
        if (sscanf(line, "widescreen=%d", &v) == 1) {
            gPortWidescreen = v != 0;
        }
        if (sscanf(line, "hud=%d", &v) == 1) {
            extern int gPortHudTop;
            gPortHudTop = v != 0;
        }
        /* measurement switches (docs/3ds-60fps-plan.md P0), hand-edited in settings.txt:
         * o3ds_sim=1: New 3DS runs at the Old 3DS clock (268 MHz, no L2) - approximates an Old 3DS
         * perf_stages=1: per-stage frame timers in boot.log (one tick read per triangle) */
        if (sscanf(line, "o3ds_sim=%d", &v) == 1) {
            extern int gPortO3dsSim;
            gPortO3dsSim = v != 0;
            sO3dsSimSetting = v != 0;
        }
        if (sscanf(line, "bench=%d", &v) == 1) {
            sBench = v != 0;
        }
        if (sscanf(line, "prof=%d", &v) == 1) {
            sProfOn = v != 0;
        }
        if (sscanf(line, "gpu_vtx=%d", &v) == 1) { /* GPU vertex path (gfx_citro3d.c), read before gfx init */
            extern int gPortGpuVtx;
            gPortGpuVtx = v != 0;
        }
        if (sscanf(line, "stereo_test=%d", &v) == 1) {
            gPortStereoTest = v != 0;
        }
        if (sscanf(line, "fps60=%d", &v) == 1) {
            sInterp = v;
        }
        if (sscanf(line, "perf_ab=%d", &v) == 1) {
            extern int gPortPerfAB;
            gPortPerfAB = v != 0;
        }
        if (sscanf(line, "perf_stages=%d", &v) == 1) {
            extern int gPortPerfStagesOn;
            gPortPerfStagesOn = v != 0;
        }
    }
    fclose(f);
}

/* PORT (2026-09-30): OoT3D-style touch panel on the bottom screen, drawn straight into its RGB565
 * framebuffer (single-buffered by consoleInit; stdout/stderr are silenced once the panel is up).
 *   left:   VIEW (C-up: first person / Navi), rupees, small keys, SCREEN (4:3 / wide), OCARINA
 *   centre: hearts + magic, the minimap (software-drawn from gPortMinimap, port_minimap.h),
 *           tabs GEAR / MAP / ITEMS (START straight to that pause page)
 *   right:  C-left (Y), C-down (ZL), C-right (X) with live item icons + ammo, BOOTS (cycles owned boots)
 * Everything redraws only when it changes. Full description: docs/3ds-touch-panel.md. */
#include "port_minimap.h"
extern const unsigned char* Port_GetItemIcon(int itemId);
extern int gPortWidescreen;

static int sTouchUi, sTouchUiRedraw, sTouchUiN3ds;
static u16* sFb;
static int sFbDirty;

#define PRGB(r, g, b) (u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define COL_TEXT   PRGB(244, 244, 244)
#define COL_SHADOW PRGB(20, 20, 24)
#define COL_DIM    PRGB(130, 130, 136)

/* item ids used by the panel (include/item.h) */
#define PANEL_ITEM_OCARINA_FAIRY   0x07
#define PANEL_ITEM_OCARINA_OF_TIME 0x08
#define PANEL_ITEM_BOOTS_KOKIRI    0x44

enum { P_VIEW, P_SCREEN, P_HUD, P_OCARINA, P_CLEFT, P_CDOWN, P_CRIGHT, P_BOOTS, P_GEAR, P_MAP, P_ITEMS, P_COUNT };
typedef struct {
    s16 x, y, w, h;
    const char* label;
    u16 btn;  /* N64 bits while held; 0 = tap action */
    s8 page;  /* pause page for the tabs (PAUSE_ITEM 0, MAP 1, EQUIP 3), -1 otherwise */
    u8 tr, tg, tb; /* tab face colour (0,0,0 = stone) */
} PanelPad;
static const PanelPad sPads[P_COUNT] = {
    { 4, 4, 56, 52, "VIEW", BTN_CUP_, -1, 0, 0, 0 },
    { 4, 112, 56, 34, "SCREEN", 0, -1, 0, 0, 0 },
    { 4, 148, 56, 32, "HUD", 0, -1, 0, 0, 0 },
    { 4, 184, 56, 52, "OCARINA", 0, -1, 0, 0, 0 },
    { 260, 4, 56, 56, "Y", BTN_CLEFT_, -1, 0, 0, 0 },
    { 260, 64, 56, 56, "ZL", BTN_CDOWN_, -1, 0, 0, 0 },
    { 260, 124, 56, 56, "X", BTN_CRIGHT_, -1, 0, 0, 0 },
    { 260, 184, 56, 52, "BOOTS", 0, -1, 0, 0, 0 },
    { 66, 208, 60, 30, "GEAR", BTN_START_, 3, 52, 116, 60 },
    { 130, 208, 60, 30, "MAP", BTN_START_, 1, 140, 44, 48 },
    { 194, 208, 60, 30, "ITEMS", BTN_START_, 0, 48, 72, 150 },
};
/* centre column */
#define MAP_X 66
#define MAP_Y 34
#define MAP_W 188
#define MAP_H 170

static inline void Px(int x, int y, u16 c) {
    if ((unsigned)x < 320 && (unsigned)y < 240) sFb[x * 240 + (239 - y)] = c;
}
static inline u16 PxGet(int x, int y) {
    return ((unsigned)x < 320 && (unsigned)y < 240) ? sFb[x * 240 + (239 - y)] : 0;
}
static inline int Noise(int x, int y) { /* stable per-pixel grain for the stone look, -8..7 */
    unsigned h = (unsigned)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int)((h >> 24) & 15) - 8;
}
static inline int Clamp8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}
static void FillRect(int x, int y, int w, int h, u16 c) {
    int i, j;
    for (i = x; i < x + w; i++) {
        for (j = y; j < y + h; j++) Px(i, j, c);
    }
}
/* textured fill: base colour + grain */
static void FillStone(int x, int y, int w, int h, int r, int g, int b, int grain) {
    int i, j;
    for (i = x; i < x + w; i++) {
        for (j = y; j < y + h; j++) {
            int n = Noise(i, j) * grain / 8;
            Px(i, j, PRGB(Clamp8(r + n), Clamp8(g + n), Clamp8(b + n)));
        }
    }
}
static u16 Blend565(u16 bg, int r, int g, int b, int a) { /* a 0..255 */
    int br = (bg >> 11) << 3, bgc = ((bg >> 5) & 63) << 2, bb = (bg & 31) << 3;
    return PRGB(br + (r - br) * a / 255, bgc + (g - bgc) * a / 255, bb + (b - bb) * a / 255);
}
/* bevelled stone button: light stone rim, face inside (OoT3D look) */
static void DrawPlate(const PanelPad* p, int pressed) {
    int x = p->x, y = p->y, w = p->w, h = p->h;
    int rim = pressed ? 150 : 196;
    int tab = p->tr | p->tg | p->tb;
    FillStone(x, y, w, h, rim, rim - 6, rim - 16, 10);
    FillRect(x, y, w, 1, PRGB(236, 232, 220));
    FillRect(x, y + h - 1, w, 1, PRGB(70, 66, 60));
    FillRect(x + w - 1, y, 1, h, PRGB(96, 92, 84));
    if (tab) {
        FillStone(x + 4, y + 4, w - 8, h - 4, pressed ? p->tr / 2 : p->tr, pressed ? p->tg / 2 : p->tg,
                  pressed ? p->tb / 2 : p->tb, 6);
    } else {
        int f = pressed ? 96 : 150;
        FillStone(x + 3, y + 3, w - 6, h - 6, f, f - 4, f - 12, 12);
    }
    Px(x, y, 0), Px(x + w - 1, y, 0), Px(x, y + h - 1, 0), Px(x + w - 1, y + h - 1, 0);
}
/* 8x8 text with the console font, transparent, 1-pixel drop shadow; scale 1 or 2 */
static void DrawTextS(int x, int y, const char* s, u16 c, int scale) {
    PrintConsole* con = consoleGetDefault();
    int pass;
    for (pass = 0; pass < 2; pass++) {
        const char* t = s;
        int cx = x + (pass == 0 ? 1 : 0), cy = y + (pass == 0 ? 1 : 0);
        u16 col = pass == 0 ? COL_SHADOW : c;
        for (; *t; t++, cx += 8 * scale) {
            unsigned ch = (unsigned char)*t;
            const u8* g;
            int r, bit;
            if (ch < con->font.asciiOffset || ch >= con->font.asciiOffset + con->font.numChars) continue;
            g = con->font.gfx + (ch - con->font.asciiOffset) * 8;
            for (r = 0; r < 8 * scale; r++) {
                for (bit = 0; bit < 8 * scale; bit++) {
                    if (g[r / scale] & (0x80 >> (bit / scale))) Px(cx + bit, cy + r, col);
                }
            }
        }
    }
}
static void DrawText(int x, int y, const char* s, u16 c) {
    DrawTextS(x, y, s, c, 1);
}
static void DrawTextC(int cx, int y, const char* s, u16 c) {
    DrawText(cx - (int)strlen(s) * 4, y, s, c);
}
/* 32x32 RGBA32 item icon at size px (nearest), alpha-blended; dim = disabled button */
static void DrawIcon(int x, int y, const u8* rgba, int size, int dim) {
    int i, j;
    if (rgba == NULL) return;
    for (j = 0; j < size; j++) {
        for (i = 0; i < size; i++) {
            /* game RAM keeps logical byte k at address k ^ 7 on the 3DS (see gfx_src_swizzle) */
            int o = ((j * 32 / size) * 32 + (i * 32 / size)) * 4;
            int r = rgba[o ^ 7], g = rgba[(o + 1) ^ 7], b = rgba[(o + 2) ^ 7], a = rgba[(o + 3) ^ 7];
            if (a == 0) continue;
            if (dim) r = g = b = (r + g + b) / 6;
            Px(x + i, y + j, Blend565(PxGet(x + i, y + j), r, g, b, a));
        }
    }
}

/* ---- panel state (what is on screen) ---- */
static int sHeldPad = -1;
static PortHudInfo sShown;
static int sShownWide = -1, sShownIconsOk = -1, sShownNavi, sShownHud = -1;
static const u8* sShownIconSeg;
static unsigned sLastHudSerial, sLastMapSerial;
static int sHudStale = 99, sMapStale = 99;

static int IconsOk(void) {
    return sHudStale < 4 && gPortHudIconSeg != NULL;
}

static void DrawPad(int i) {
    const PanelPad* p = &sPads[i];
    int cx = p->x + p->w / 2;
    char buf[8];
    DrawPlate(p, i == sHeldPad);
    if (i >= P_CLEFT && i <= P_CRIGHT) {
        int c = i - P_CLEFT;
        if (sShown.cItem[c] < 0x56 && IconsOk()) {
            DrawIcon(cx - 22, p->y + 5, gPortHudIconSeg + (c + 1) * 32 * 32 * 4, 44, sShown.cDisabled[c]);
            if (sShown.cAmmo[c] >= 0) {
                snprintf(buf, sizeof(buf), "%d", sShown.cAmmo[c]);
                DrawText(p->x + p->w - 5 - 8 * (int)strlen(buf), p->y + p->h - 13, buf,
                         sShown.cAmmo[c] == 0 ? PRGB(255, 90, 60) : PRGB(120, 250, 120));
            }
        }
        /* the 3DS button that also presses it (ZL only exists on the New 3DS) */
        if (c != 1 || sTouchUiN3ds) DrawText(p->x + 4, p->y + 4, p->label, PRGB(255, 230, 120));
    } else if (i == P_BOOTS) {
        if (sShown.boots >= 1 && sShown.boots <= 3) {
            DrawIcon(cx - 20, p->y + 3, Port_GetItemIcon(PANEL_ITEM_BOOTS_KOKIRI + sShown.boots - 1), 40, 0);
        }
        DrawTextC(cx, p->y + p->h - 12, "BOOTS", COL_TEXT);
    } else if (i == P_OCARINA) {
        int have = sShown.ocarina == PANEL_ITEM_OCARINA_FAIRY || sShown.ocarina == PANEL_ITEM_OCARINA_OF_TIME;
        if (have) DrawIcon(cx - 20, p->y + 3, Port_GetItemIcon(sShown.ocarina), 40, 0);
        DrawTextC(cx, p->y + p->h - 12, "OCARINA", have ? COL_TEXT : COL_DIM);
    } else if (i == P_VIEW && sShownNavi) {
        /* Navi wants to talk (OoT3D swaps the VIEW eye for her): a glowing fairy, "NAVI" */
        int fy = p->y + 20, dx, dy;
        for (dy = -16; dy <= 16; dy++) {
            for (dx = -24; dx <= 24; dx++) {
                int d2 = dx * dx + dy * dy;
                /* wings: two tilted ellipses on each side */
                int wx = dx < 0 ? -dx : dx, wyU = dy + 5, wyD = dy - 7;
                if ((wx > 5 && (wx - 13) * (wx - 13) * 9 + wyU * wyU * 49 <= 9 * 49 * 4) ||
                    (wx > 5 && (wx - 11) * (wx - 11) * 16 + wyD * wyD * 64 <= 16 * 64 * 2)) {
                    Px(cx + dx, fy + dy, Blend565(PxGet(cx + dx, fy + dy), 200, 235, 255, 150));
                }
                if (d2 <= 144) { /* glow */
                    int a = 255 - d2 * 255 / 144;
                    Px(cx + dx, fy + dy, Blend565(PxGet(cx + dx, fy + dy), 120, 200, 255, a));
                }
                if (d2 <= 16) Px(cx + dx, fy + dy, PRGB(250, 255, 255));
            }
        }
        DrawTextC(cx, p->y + p->h - 13, "NAVI", PRGB(150, 220, 255));
    } else if (i == P_VIEW) {
        /* an eye, like OoT3D's VIEW button */
        int ey = p->y + 21, dx, dy;
        for (dy = -9; dy <= 9; dy++) {
            for (dx = -18; dx <= 18; dx++) {
                if (dx * dx * 81 + dy * dy * 324 <= 81 * 324) Px(cx + dx, ey + dy, PRGB(240, 240, 248));
                if (dx * dx + dy * dy <= 49) Px(cx + dx, ey + dy, PRGB(100, 80, 200));
                if (dx * dx + dy * dy <= 9) Px(cx + dx, ey + dy, PRGB(16, 16, 32));
            }
        }
        DrawTextC(cx, p->y + p->h - 13, "VIEW", COL_TEXT);
    } else if (i == P_SCREEN) {
        DrawTextC(cx, p->y + 6, "SCREEN", COL_TEXT);
        DrawTextC(cx, p->y + 19, gPortWidescreen ? "WIDE" : "4:3", PRGB(255, 230, 120));
    } else if (i == P_HUD) {
        DrawTextC(cx, p->y + 5, "TOP HUD", COL_TEXT);
        DrawTextC(cx, p->y + 18, gPortHudTop ? "ON" : "OFF", PRGB(255, 230, 120));
    } else {
        /* tab: bold label */
        DrawText(cx - (int)strlen(p->label) * 4, p->y + 12, p->label, COL_TEXT);
        DrawText(cx - (int)strlen(p->label) * 4 + 1, p->y + 12, p->label, COL_TEXT);
    }
    sFbDirty = 1;
}

static void DrawHeart(int x, int y, int fill16) { /* 9x8 heart, fill in 16ths (quarters shown) */
    static const char* const sShape[8] = { ".XX.XX..", "XXXXXXX.", "XXXXXXX.", "XXXXXXX.",
                                           ".XXXXX..", "..XXX...", "...X....", "........" };
    int r, c;
    for (r = 0; r < 7; r++) {
        for (c = 0; c < 7; c++) {
            if (sShape[r][c] != 'X') continue;
            /* quarters: fill from the bottom-left quadrant clockwise, approximated by columns */
            int filled = fill16 >= 16 || (fill16 > 0 && c < (fill16 * 7 + 15) / 16);
            Px(x + c, y + r, filled ? PRGB(230, 30, 40) : PRGB(70, 40, 44));
        }
    }
}

static void DrawStatus(void) {
    int i, hearts;
    FillStone(MAP_X, 0, MAP_W, MAP_Y - 2, 26, 22, 20, 4);
    if (sShown.valid) {
        hearts = sShown.healthCapacity / 16;
        for (i = 0; i < hearts && i < 20; i++) {
            int fill = sShown.health - i * 16;
            DrawHeart(MAP_X + 6 + (i % 10) * 10, 4 + (i / 10) * 9, fill < 0 ? 0 : (fill > 16 ? 16 : fill));
        }
        if (sShown.magicCapacity > 0) {
            int w = sShown.magicCapacity * (MAP_W - 12) / 96; /* 48 = single, 96 = double magic */
            int f = sShown.magic * (MAP_W - 12) / 96;
            FillRect(MAP_X + 5, 24, w + 2, 6, PRGB(230, 230, 230));
            FillRect(MAP_X + 6, 25, w, 4, PRGB(20, 20, 20));
            FillRect(MAP_X + 6, 25, f, 4, PRGB(40, 200, 60));
        }
    }
    sFbDirty = 1;
}

static void DrawCounters(void) {
    char buf[16];
    FillStone(0, 60, 64, 52, 40, 42, 46, 6);
    if (sShown.rupees >= 0) {
        int dy;
        for (dy = 0; dy < 14; dy++) { /* green rupee */
            int half = dy < 4 ? dy + 2 : (dy < 10 ? 5 : 14 - dy + 1);
            FillRect(14 - half, 66 + dy, half * 2, 1, dy < 7 ? PRGB(90, 230, 110) : PRGB(40, 170, 60));
        }
        snprintf(buf, sizeof(buf), "%d", sShown.rupees);
        DrawTextS(24, 66, buf, PRGB(150, 250, 150), sShown.rupees < 1000 ? 1 : 1);
    }
    if (sShown.keys >= 0) {
        FillRect(8, 88, 7, 7, PRGB(210, 210, 220)); /* small key */
        FillRect(10, 90, 3, 3, PRGB(40, 42, 46));
        FillRect(10, 95, 3, 9, PRGB(210, 210, 220));
        FillRect(13, 99, 3, 2, PRGB(210, 210, 220));
        FillRect(13, 102, 3, 2, PRGB(210, 210, 220));
        snprintf(buf, sizeof(buf), "%d", sShown.keys);
        DrawText(24, 92, buf, COL_TEXT);
    }
    sFbDirty = 1;
}

/* ---- minimap ---- */
static void MapTri(float cx, float cy, short yaw, float size, u16 col) {
    /* arrow along yaw: world +x is right and +z is down on the map */
    float a = yaw * (3.14159265f / 32768.0f);
    float fx = sinf(a), fy = cosf(a);
    float px[3] = { cx + fx * size, cx - fx * size * 0.6f + fy * size * 0.6f, cx - fx * size * 0.6f - fy * size * 0.6f };
    float py[3] = { cy + fy * size, cy - fy * size * 0.6f - fx * size * 0.6f, cy - fy * size * 0.6f + fx * size * 0.6f };
    int x0 = (int)floorf(fminf(px[0], fminf(px[1], px[2]))), x1 = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
    int y0 = (int)floorf(fminf(py[0], fminf(py[1], py[2]))), y1 = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
    int x, y, k;
    for (y = y0 - 1; y <= y1 + 1; y++) {
        for (x = x0 - 1; x <= x1 + 1; x++) {
            int pos = 0, neg = 0;
            for (k = 0; k < 3; k++) {
                int n = (k + 1) % 3;
                float e = (px[n] - px[k]) * (y + 0.5f - py[k]) - (py[n] - py[k]) * (x + 0.5f - px[k]);
                if (e >= 0) pos++; else neg++;
            }
            if ((pos == 3 || neg == 3) && x >= MAP_X && x < MAP_X + MAP_W && y >= MAP_Y && y < MAP_Y + MAP_H) {
                Px(x, y, col);
            }
        }
    }
}

static int MapTexel(const PortMinimap* m, int tx, int ty, int* lum) { /* alpha 0..255 */
    const u8* t = m->tex;
    int idx = ty * m->w + tx;
    int nib = (t[(idx >> 1) ^ 7] >> ((idx & 1) ? 0 : 4)) & 0xF; /* byte k at k ^ 7 */
    if (m->fmt == PORT_MINIMAP_I4) {
        *lum = 255;
        return nib * 17;
    }
    *lum = (nib >> 1) * 255 / 7;
    return (nib & 1) ? 255 : 0;
}

static void DrawMapInto(int have);

/* The bottom framebuffer is single-buffered and scanned out while we draw: repainting the parchment and
 * then the map in place showed the half-drawn state (the map flashed on hardware). Draw into a shadow
 * copy with the framebuffer's layout, then copy only the map columns over in one pass. */
static void DrawMap(int have) {
    static u16 sShadow[320 * 240];
    u16* real = sFb;
    int x;
    sFb = sShadow;
    DrawMapInto(have);
    sFb = real;
    for (x = MAP_X; x < MAP_X + MAP_W; x++) {
        /* column x holds rows 239..0; the map's rows MAP_Y..MAP_Y+MAP_H-1 are contiguous in it */
        int base = x * 240 + (239 - (MAP_Y + MAP_H - 1));
        memcpy(&real[base], &sShadow[base], MAP_H * sizeof(u16));
    }
    sFbDirty = 1;
}

/* PORT PERF (2026-10-01): the parchment (noise per pixel) and the resampled map texture only change
 * with the room, but were repainted on every redraw - every other frame while Link moves (hardware
 * profile: the panel took 2-7% of Old 3DS time). They are cached here, keyed by what they depend on;
 * a redraw copies them and paints only the markers. */
static u16 sMapBase[320 * 240];
static u32 sMapBaseKey;
static int sMapBaseValid;
static float sMapBaseS, sMapBaseOx, sMapBaseOy;

static void MapCopyColumns(u16* dst, const u16* src) {
    int x;
    for (x = MAP_X; x < MAP_X + MAP_W; x++) {
        int base = x * 240 + (239 - (MAP_Y + MAP_H - 1));
        memcpy(&dst[base], &src[base], MAP_H * sizeof(u16));
    }
}

static void DrawMapBase(int have, float* ps, float* pox, float* poy);

static void DrawMapInto(int have) {
    const PortMinimap* m = &gPortMinimap;
    float s, ox, oy;
    int x;
    u32 key = (u32)have * 0x9E3779B1u ^ (u32)(uintptr_t)m->tex * 31u ^ (u32)m->w * 7u ^ (u32)m->h * 13u ^
              (u32)m->r * 29u ^ (u32)m->g * 37u ^ (u32)m->b * 41u;
    if (have && m->tex != NULL && m->w > 0 && m->h > 0) { /* same buffer, new room: sample the texels */
        const u8* t = m->tex;
        int n = m->w * m->h / 2, k;
        for (k = 0; k < 64; k++) key = key * 33u + t[(k * (n / 64)) ^ 7];
    }
    if (!sMapBaseValid || key != sMapBaseKey) {
        DrawMapBase(have, &sMapBaseS, &sMapBaseOx, &sMapBaseOy);
        MapCopyColumns(sMapBase, sFb);
        sMapBaseKey = key;
        sMapBaseValid = 1;
    } else {
        MapCopyColumns(sFb, sMapBase);
    }
    sFbDirty = 1;
    if (!have || m->w <= 0 || m->h <= 0) return;
    s = sMapBaseS, ox = sMapBaseOx, oy = sMapBaseOy;
#define MX(v) (ox + ((v) - m->x) * s)
#define MY(v) (oy + ((v) - m->y) * s)
    for (x = 0; x < m->numIcons; x++) {
        int ix = (int)MX(m->iconX[x]), iy = (int)MY(m->iconY[x]);
        FillRect(ix, iy, 8, 8, PRGB(60, 30, 20));
        FillRect(ix + 1, iy + 1, 6, 6, PRGB(220, 70, 40));
    }
    for (x = 0; x < m->numMarks; x++) {
        int mx = (int)MX(m->markX[x]), my = (int)MY(m->markY[x]);
        if (m->markType[x] == 0 /* MAP_MARK_CHEST */) {
            FillRect(mx, my, 9, 7, PRGB(70, 36, 16));
            FillRect(mx + 1, my + 1, 7, 5, PRGB(210, 60, 40));
            FillRect(mx + 1, my + 3, 7, 1, PRGB(240, 200, 60));
        } else { /* boss */
            FillRect(mx, my, 9, 9, PRGB(30, 20, 20));
            FillRect(mx + 1, my + 1, 7, 7, PRGB(240, 240, 240));
            FillRect(mx + 2, my + 3, 2, 2, PRGB(200, 20, 20));
            FillRect(mx + 5, my + 3, 2, 2, PRGB(200, 20, 20));
        }
    }
    if (m->compass) {
        MapTri(MX(m->startX), MY(m->startY), m->startYaw, 7, PRGB(200, 0, 0));
        MapTri(MX(m->playerX), MY(m->playerY), m->playerYaw, 9, PRGB(40, 40, 20));
        MapTri(MX(m->playerX), MY(m->playerY), m->playerYaw, 7, PRGB(250, 240, 0));
    }
#undef MX
#undef MY
}

/* the parchment and the map texture, fitted to the frame (its scale/offset returned for the markers) */
static void DrawMapBase(int have, float* ps, float* pox, float* poy) {
    const PortMinimap* m = &gPortMinimap;
    float s, ox, oy, inv;
    int x, y, bx0, by0, bx1, by1, lum;
    *ps = 1.0f, *pox = 0.0f, *poy = 0.0f;
    /* parchment with a darker burnt edge */
    for (x = MAP_X; x < MAP_X + MAP_W; x++) {
        for (y = MAP_Y; y < MAP_Y + MAP_H; y++) {
            int e = x - MAP_X, d;
            if (MAP_X + MAP_W - 1 - x < e) e = MAP_X + MAP_W - 1 - x;
            if (y - MAP_Y < e) e = y - MAP_Y;
            if (MAP_Y + MAP_H - 1 - y < e) e = MAP_Y + MAP_H - 1 - y;
            d = e < 10 ? (10 - e) * 7 : 0;
            {
                int n = Noise(x >> 1, y >> 1) * 2;
                Px(x, y, PRGB(Clamp8(206 - d + n), Clamp8(176 - d * 5 / 4 + n), Clamp8(116 - d * 3 / 2 + n)));
            }
        }
    }
    sFbDirty = 1;
    if (!have || m->w <= 0 || m->h <= 0) return;
    /* fit the drawn part of the texture (plus the markers) to the frame: N64 maps sit in a corner of
     * their texture, OoT3D shows them large and centred */
    bx0 = m->w, by0 = m->h, bx1 = -1, by1 = -1;
    if (m->tex != NULL) {
        for (y = 0; y < m->h; y++) {
            for (x = 0; x < m->w; x++) {
                if (MapTexel(m, x, y, &lum) != 0) {
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
            }
        }
    }
    if (bx1 < 0) bx0 = 0, by0 = 0, bx1 = m->w - 1, by1 = m->h - 1; /* nothing drawn: whole frame */
    bx1++, by1++;
    s = fminf((MAP_W - 16) / (float)(bx1 - bx0), (MAP_H - 16) / (float)(by1 - by0));
    if (s > 3.0f) s = 3.0f;
    ox = MAP_X + (MAP_W - (bx1 - bx0) * s) * 0.5f - bx0 * s;
    oy = MAP_Y + (MAP_H - (by1 - by0) * s) * 0.5f - by0 * s;
    inv = 1.0f / s;
    if (m->tex != NULL) {
        /* the game's colours are for a dark screen: darken them onto the parchment */
        int r = m->r * 2 / 5, g = m->g * 2 / 5, b = m->b * 2 / 5 + 70;
        for (y = MAP_Y + 2; y < MAP_Y + MAP_H - 2; y++) {
            int ty = (int)floorf((y + 0.5f - oy) * inv);
            if (ty < by0 || ty >= by1) continue;
            for (x = MAP_X + 2; x < MAP_X + MAP_W - 2; x++) {
                int tx = (int)floorf((x + 0.5f - ox) * inv), a;
                if (tx < bx0 || tx >= bx1) continue;
                a = MapTexel(m, tx, ty, &lum);
                if (a == 0) continue;
                Px(x, y, Blend565(PxGet(x, y), r * lum / 255, g * lum / 255, b * lum / 255, a));
            }
        }
    }
    *ps = s, *pox = ox, *poy = oy;
}

static void Port3ds_TouchUiInit(void) {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    { extern void PortCompat_SilenceStderr(void); PortCompat_SilenceStderr(); }
    sTouchUi = 1;
    sTouchUiN3ds = n3ds;
    sTouchUiRedraw = 1; /* drawn on the first poll: renderer init (in the game loop) clears the screen */
}

/* held pads -> N64 bits; tap actions; redraws what changed */
static unsigned short Port3ds_TouchUiPoll(void) {
    static unsigned sPolls;
    int hit = -1, i, iconsOk;
    PortHudInfo now;
    if (!sTouchUi) return 0;
    sPolls++;
    sFb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    if (gPortTouchOcarina > 0) gPortTouchOcarina--; /* requests expire if the player could not act */
    if (gPortTouchBoots > 0) gPortTouchBoots--;

    /* freshness of the game-side data */
    sHudStale = (gPortHudSerial != sLastHudSerial) ? 0 : sHudStale + 1;
    sLastHudSerial = gPortHudSerial;
    sMapStale = (gPortMinimap.serial != sLastMapSerial) ? 0 : sMapStale + 1;
    sLastMapSerial = gPortMinimap.serial;

    Port_GetHudInfo(&now);
    if (sHudStale >= 4) now.keys = -1; /* the HUD is not being drawn (title, file select) */

    if (sTouchUiRedraw) {
        sTouchUiRedraw = 0;
        FillStone(0, 0, 320, 240, 40, 42, 46, 6);
        FillStone(MAP_X - 2, 0, MAP_W + 4, 240, 26, 22, 20, 4);
        sShown = now;
        sShownIconsOk = IconsOk();
        sShownIconSeg = gPortHudIconSeg;
        sShownWide = gPortWidescreen;
        for (i = 0; i < P_COUNT; i++) DrawPad(i);
        DrawStatus();
        DrawCounters();
        DrawMap(0);
    }

    if (hidKeysHeld() & KEY_TOUCH) {
        touchPosition tp;
        hidTouchRead(&tp);
        for (i = 0; i < P_COUNT; i++) {
            const PanelPad* p = &sPads[i];
            if (tp.px >= p->x && tp.px < p->x + p->w && tp.py >= p->y && tp.py < p->y + p->h) hit = i;
        }
    }
    if (hidKeysDown() & KEY_TOUCH) {
        if (hit == P_SCREEN) {
            gPortWidescreen = !gPortWidescreen;
            Port3ds_SaveSettings();
        } else if (hit == P_HUD) {
            gPortHudTop = !gPortHudTop;
            Port3ds_SaveSettings();
        } else if (hit == P_OCARINA) {
            gPortTouchOcarina = 3;
        } else if (hit == P_BOOTS) {
            gPortTouchBoots = 3;
        } else if (hit >= 0 && sPads[hit].page >= 0) {
            gPortTouchPage = sPads[hit].page;
        }
    }
    if (hit != sHeldPad) {
        int old = sHeldPad;
        sHeldPad = hit;
        if (old >= 0) DrawPad(old);
        if (hit >= 0) DrawPad(hit);
    }

    iconsOk = IconsOk();
    /* C pads: item/ammo/disabled changes, and a few refreshes a second while icons may still be
     * arriving by async DMA after an equip */
    if (memcmp(now.cItem, sShown.cItem, 3) || memcmp(now.cDisabled, sShown.cDisabled, 3) ||
        memcmp(now.cAmmo, sShown.cAmmo, sizeof(now.cAmmo)) || iconsOk != sShownIconsOk ||
        (iconsOk && (sShownIconSeg != gPortHudIconSeg || (sPolls % 20) == 0))) {
        memcpy(sShown.cItem, now.cItem, 3);
        memcpy(sShown.cDisabled, now.cDisabled, 3);
        memcpy(sShown.cAmmo, now.cAmmo, sizeof(now.cAmmo));
        sShownIconsOk = iconsOk;
        sShownIconSeg = gPortHudIconSeg;
        for (i = 0; i < 3; i++) DrawPad(P_CLEFT + i);
    }
    if (now.boots != sShown.boots) sShown.boots = now.boots, DrawPad(P_BOOTS);
    if (now.ocarina != sShown.ocarina) sShown.ocarina = now.ocarina, DrawPad(P_OCARINA);
    if (gPortWidescreen != sShownWide) sShownWide = gPortWidescreen, DrawPad(P_SCREEN);
    if (gPortHudTop != sShownHud) sShownHud = gPortHudTop, DrawPad(P_HUD);
    {
        int navi = gPortHudNavi && sHudStale < 4;
        if (navi != sShownNavi) sShownNavi = navi, DrawPad(P_VIEW);
    }
    if (now.rupees != sShown.rupees || now.keys != sShown.keys) {
        sShown.rupees = now.rupees, sShown.keys = now.keys;
        DrawCounters();
    }
    if (now.valid != sShown.valid || now.health != sShown.health || now.healthCapacity != sShown.healthCapacity ||
        now.magic != sShown.magic || now.magicCapacity != sShown.magicCapacity) {
        sShown.valid = now.valid, sShown.health = now.health, sShown.healthCapacity = now.healthCapacity;
        sShown.magic = now.magic, sShown.magicCapacity = now.magicCapacity;
        DrawStatus();
    }

    { /* minimap: redrawn every other poll while the game feeds it, cleared once when it stops */
        static int sMapShown;
        static u32 sMapSig;
        if (sMapStale == 0 && (sPolls & 1)) {
            /* redraw only when something visible changed (arrows move by whole panel pixels) */
            const PortMinimap* m = &gPortMinimap;
            u32 sig = (u32)(uintptr_t)m->tex * 31u + (u32)m->w * 7u + (u32)m->h * 13u + m->compass * 17u +
                      (u32)(int)(m->playerX * 2.0f) * 101u + (u32)(int)(m->playerY * 2.0f) * 1009u +
                      (u32)(m->playerYaw >> 11) * 10007u + (u32)(int)m->startX * 3u + (u32)(int)m->startY * 5u +
                      m->numMarks * 19u + m->numIcons * 23u + (u32)m->r * 29u + (u32)m->g * 37u;
            if (m->tex != NULL && m->w > 0 && m->h > 0) { /* same buffer, new room: sample the texels */
                const u8* t = m->tex;
                int n = m->w * m->h / 2, k;
                for (k = 0; k < 64; k++) sig = sig * 33u + t[(k * (n / 64)) ^ 7];
            }
            if (!sMapShown || sig != sMapSig) {
                DrawMap(1);
                sMapSig = sig;
            }
            sMapShown = 1;
        } else if (sMapStale > 6 && sMapShown) {
            DrawMap(0);
            sMapShown = 0;
        }
    }

    if (sFbDirty) {
        sFbDirty = 0;
        GSPGPU_FlushDataCache(sFb, 240 * 320 * 2);
    }

    { /* verification aid: with sdmc:/3ds/oot/capture_bottom present, dump the panel every 300 polls
       * (raw framebuffer + header, overwritten) so tools can check it without screenshots */
        if ((sPolls % 300) == 0) {
            FILE* flag = fopen("sdmc:/3ds/oot/capture_bottom", "rb");
            if (flag != NULL) {
                u16 w, h;
                u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
                FILE* out = fopen("sdmc:/3ds/oot/bottom_fb.bin", "wb");
                fclose(flag);
                if (out != NULL) {
                    u32 hdr[3] = { w, h, (u32)gfxGetScreenFormat(GFX_BOTTOM) };
                    fwrite(hdr, 4, 3, out);
                    fwrite(fb, 1, (size_t)w * h * gspGetBytesPerPixel(gfxGetScreenFormat(GFX_BOTTOM)), out);
                    fclose(out);
                }
            }
        }
    }
    /* the tabs press START only when opening a page from gameplay (or closing the pause menu) */
    return hit >= 0 ? sPads[hit].btn : 0;
}


static void Port3ds_PollInput(void) {
    hidScanInput();
    u32 k = hidKeysHeld();
    /* PORT (2026-09-30): OoT3D-style layout, complete on the Old 3DS (no ZL/ZR, no C-stick):
     *   A/B = A/B, L = Z-target, R = shield, Y/X = C-left/C-right, D-pad = C-up/C-down/C-left/C-right
     *   (the N64 D-pad is unused by OoT), SELECT = N64 L (minimap), START = START, touch panel =
     *   VIEW/C buttons/OCARINA/BOOTS/pause tabs/SCREEN + the minimap (docs/3ds-touch-panel.md).
     * New 3DS extras: C-stick = the four C buttons, ZL = C-down (third item), ZR = C-up (first-person /
     * Navi), so every item and the look view are reachable while moving. */
    unsigned short b = 0;
    if (k & KEY_A)      b |= BTN_A_;
    if (k & KEY_B)      b |= BTN_B_;
    if (k & KEY_Y)      b |= BTN_CLEFT_;
    if (k & KEY_X)      b |= BTN_CRIGHT_;
    if (k & KEY_START)  b |= BTN_START_;
    if (k & KEY_SELECT) b |= BTN_L_;
    if (k & KEY_L)      b |= BTN_Z_;
    if (k & KEY_R)      b |= BTN_R_;
    if (k & KEY_ZL)     b |= BTN_CDOWN_;
    if (k & KEY_ZR)     b |= BTN_CUP_;
    if (k & KEY_DUP)    b |= BTN_CUP_;
    if (k & KEY_DDOWN)  b |= BTN_CDOWN_;
    if (k & KEY_DLEFT)  b |= BTN_CLEFT_;
    if (k & KEY_DRIGHT) b |= BTN_CRIGHT_;
    /* C-stick (New 3DS) -> C buttons */
    if (k & KEY_CSTICK_UP)    b |= BTN_CUP_;
    if (k & KEY_CSTICK_DOWN)  b |= BTN_CDOWN_;
    if (k & KEY_CSTICK_LEFT)  b |= BTN_CLEFT_;
    if (k & KEY_CSTICK_RIGHT) b |= BTN_CRIGHT_;
    b |= Port3ds_TouchUiPoll();

    circlePosition cp;
    hidCircleRead(&cp);
    /* circle pad range ~ +-156; scale to N64 +-80 */
    s3dsStickX = (signed char)(cp.dx * 80 / 156);
    s3dsStickY = (signed char)(cp.dy * 80 / 156);
    s3dsButtons = b;
}

/* render hooks — override sched_shim's weak PortGfx_RunTask (the 3DS analogue
 * of pc_gfx.c). Drives the citro3d backend via the gfx_pc interface. */
static int sGfxInited = 0;

void PortGfx_Init(void) {
    gfx_init(&gfx_3ds, &gfx_citro3d_api);
    sGfxInited = 1;
    { extern void PortDbg(const char*); PortDbg("[gfx] citro3d renderer initialized"); }
}

void PortGfx_FrameReady(void) {}

/* PORT (2026-09-24): N64-faithful pacing. OoT advances its logic once every R_UPDATE_RATE VI
 * retraces (3 -> 20 updates/s) and the N64 AudioMgr runs on EVERY retrace (60/s), independent of
 * the game. The port used to run one update per rendered frame (measured 26-29/s -> game ~1.4x too
 * fast, speed varying with scene load) and pumped audio once per update. Here: after presenting,
 * wait whole retraces until R_UPDATE_RATE retrace periods have passed since the previous update,
 * pumping audio once per retrace. A frame that renders late simply runs late (like N64 lag). */
static unsigned sPortAudioPumps = 0;
/* PORT PERF (2026-09-30): audio pumps also run DURING rendering. On the N64 the audio thread preempts the
 * game whenever a retrace arrives; here pumps only ran after the frame, so a slow frame (Old 3DS: 100 ms)
 * caught up with 5-6 back-to-back pumps, each waiting for the previous microcode task on the worker
 * core (measured 17-31 ms of main-thread waiting per frame, O3DS-sim). The display-list walker calls
 * Port3ds_MaybePumpAudio every 256 commands: one pump per elapsed retrace, so the worker's microcode
 * overlaps the rest of the frame. The display list is complete by then; audio only touches audio state. */
static double sLastPumpMs;  /* time of the last pump (any source) */
static int sMidFramePumps;  /* pumps since the previous Port3ds_PaceFrame */
#define PORT_RETRACE_MS (1000.0 / 59.83)
void Port3ds_MaybePumpAudio(void) {
    extern void Port3ds_PumpAudio(void);
    double now = (double)osGetTime();
    if (sLastPumpMs != 0.0 && now - sLastPumpMs >= PORT_RETRACE_MS) {
        Port3ds_PumpAudio();
        sMidFramePumps++;
        sLastPumpMs += PORT_RETRACE_MS;
        if (now - sLastPumpMs > 4 * PORT_RETRACE_MS) sLastPumpMs = now; /* far behind: don't burst */
    }
}
static u64 sLast = 0; /* when the previous logic frame was presented (retrace-aligned) */
static int Port3ds_UpdateRate(void) {
    extern void* gRegEditor;
    int rate = 3;
    if (gRegEditor) rate = *(short*)((char*)gRegEditor + 0x14 + 126 * 2); /* R_UPDATE_RATE = SREG(30) */
    if (rate < 1) rate = 1;
    if (rate > 6) rate = 6;
    return rate;
}
/* wait whole retraces (pumping audio on each) until `targetMs` is under one retrace away */
static void Port3ds_WaitUntil(double targetMs) {
    extern void Port3ds_PumpAudio(void);
    while ((double)osGetTime() + 2.0 < targetMs) {
        gspWaitForVBlank();
        Port3ds_PumpAudio();
        sLastPumpMs = (double)osGetTime();
        sMidFramePumps++;
    }
}
static void Port3ds_PaceFrame(void) {
    extern void* gRegEditor;
    extern void Port3ds_PumpAudio(void);
    const double kRetraceMs = 1000.0 / 59.83; /* 3DS LCD refresh */
    int rate = 3;
    if (gRegEditor) rate = *(short*)((char*)gRegEditor + 0x14 + 126 * 2); /* R_UPDATE_RATE = SREG(30) */
    if (rate < 1) rate = 1;
    if (rate > 6) rate = 6;
    int pumped = sMidFramePumps; /* already done during rendering */
    u64 now;
    sMidFramePumps = 0;
    for (;;) {
        /* PORT (2026-09-30): check the budget BEFORE waiting. The loop used to wait for a retrace
         * first, so a frame that had already used its budget (hardware: ~44 ms of work vs 50 ms)
         * still waited up to one more retrace: measured pace 14.7 ms/frame, 17.1 updates/s on a New
         * 3DS instead of 20. A late frame now goes straight on (audio catches up below). */
        if (sLast != 0 && (double)(osGetTime() - sLast) >= rate * kRetraceMs) break;
        gspWaitForVBlank();
        Port3ds_PumpAudio(); /* build+dispatch one audio RSP task per retrace, as on N64 */
        sLastPumpMs = (double)osGetTime();
        pumped++;
        if (sLast == 0 || (double)(osGetTime() - sLast) + 2.0 >= rate * kRetraceMs) break;
    }
    now = osGetTime();
    /* Audio runs on wall-clock retraces, not on how many waits happened: if rendering ate most of
     * the budget, catch up to one audio task per retrace actually elapsed (capped). */
    if (sLast != 0) {
        int due = (int)((double)(now - sLast) / kRetraceMs + 0.5);
        if (due > 8) due = 8;
        while (pumped < due) { Port3ds_PumpAudio(); pumped++; }
        sLastPumpMs = (double)osGetTime();
    }
    sPortAudioPumps += (unsigned)pumped;
    sLast = now;
}

/* PORT PERF (2026-09-28): per-frame CPU breakdown in 268 MHz system ticks, logged every 300 frames as
 * average microseconds per frame: game = everything between two graph tasks (game logic + DL build),
 * dl = display-list interpretation (gfx_run), swap = frame end/GPU wait, pace = retrace waits + audio;
 * plus triangles and draw calls sent to the GPU per frame (gfx_pc.c counters). */
u32 gPortPerfTris, gPortPerfDraws, gPortPerfTrisIn;
u64 gPortPerfAudioMain;
static u64 sPerfGame, sPerfDl, sPerfSwap, sPerfPace, sPerfLastEnd;
static void Port3ds_PerfReport(unsigned frames) {
    extern void PortDbgX(const char*, unsigned);
    const u64 div = (u64)frames * (SYSCLOCK_ARM11 / 1000000); /* ticks -> us per frame */
    PortDbgX("perf us/frame game", (unsigned)(sPerfGame / div));
    {
        extern u64 gPortPerfGpuWait;
        PortDbgX("perf us/frame dl (cpu)", (unsigned)((sPerfDl - gPortPerfGpuWait) / div));
        PortDbgX("perf us/frame gpu wait", (unsigned)(gPortPerfGpuWait / div));
        gPortPerfGpuWait = 0;
    }
    {
        extern u64 gPortPerfTex, gPortPerfVtx, gPortPerfTri, gPortPerfFlush;
        extern u32 gPortPerfTexImports;
        PortDbgX("perf us/frame  dl.tex", (unsigned)(gPortPerfTex / div));
        PortDbgX("perf us/frame  dl.vtx", (unsigned)(gPortPerfVtx / div));
        PortDbgX("perf us/frame  dl.tri", (unsigned)(gPortPerfTri / div));
        PortDbgX("perf us/frame  dl.flush", (unsigned)(gPortPerfFlush / div));
        {
            extern u64 gPortPerfEmit, gPortPerfMtx;
            PortDbgX("perf us/frame  dl.tri.emit (clip+pack)", (unsigned)(gPortPerfEmit / div));
            PortDbgX("perf us/frame  dl.mtx", (unsigned)(gPortPerfMtx / div));
            gPortPerfEmit = gPortPerfMtx = 0;
        }
        PortDbgX("perf tex imports/frame", gPortPerfTexImports / frames);
        gPortPerfTex = gPortPerfVtx = gPortPerfTri = gPortPerfFlush = 0;
        gPortPerfTexImports = 0;
    }
    PortDbgX("perf us/frame swap", (unsigned)(sPerfSwap / div));
    PortDbgX("perf us/frame pace", (unsigned)(sPerfPace / div));
    {
        extern u64 gPortPerfAudioUcode, gPortPerfAudioWait, gPortPerfAudioMain;
        extern u32 gPortPerfAudioTasks;
        PortDbgX("perf us/frame  audio main (engine+wait)", (unsigned)(gPortPerfAudioMain / div));
        PortDbgX("perf us/frame  audio wait for ucode", (unsigned)(gPortPerfAudioWait / div));
        PortDbgX("perf us/task   audio ucode (worker)",
                 gPortPerfAudioTasks ? (unsigned)(gPortPerfAudioUcode / gPortPerfAudioTasks / (SYSCLOCK_ARM11 / 1000000)) : 0);
        gPortPerfAudioUcode = gPortPerfAudioWait = gPortPerfAudioMain = 0;
        { /* perf_stages: the 5 most expensive microcode ops, as op << 20 | us per frame */
            extern u64 gPortPerfAudOpTicks[32];
            extern int gPortPerfStagesOn;
            int k, i;
            for (k = 0; k < 5 && gPortPerfStagesOn; k++) {
                int best = 0;
                for (i = 1; i < 32; i++) if (gPortPerfAudOpTicks[i] > gPortPerfAudOpTicks[best]) best = i;
                if (gPortPerfAudOpTicks[best] == 0) break;
                PortDbgX("perf audio op<<20|us/frame", ((unsigned)best << 20) | (unsigned)(gPortPerfAudOpTicks[best] / div));
                gPortPerfAudOpTicks[best] = 0;
            }
            memset(gPortPerfAudOpTicks, 0, sizeof(gPortPerfAudOpTicks));
        }
        gPortPerfAudioTasks = 0;
    }
    PortDbgX("perf tris/frame", gPortPerfTris / frames);
    PortDbgX("perf tris in/frame (before clip+subdivision)", gPortPerfTrisIn / frames);
    { extern u32 gPortPerfSlowTris; PortDbgX("perf tris slow-path/frame", gPortPerfSlowTris / frames); gPortPerfSlowTris = 0; }
    gPortPerfTrisIn = 0;
    { /* memory budget (Old 3DS target: 96MB mode): linear free, regular heap in use (KB) */
        extern u32 linearSpaceFree(void);
        extern char* fake_heap_start;
        extern char* fake_heap_end;
        struct mallinfo mi = mallinfo();
        static u32 sMinLinearFree = 0xFFFFFFFF;
        u32 lf = linearSpaceFree();
        if (lf < sMinLinearFree) sMinLinearFree = lf;
        PortDbgX("mem linear free KB", lf / 1024);
        PortDbgX("mem linear free min KB", sMinLinearFree / 1024);
        PortDbgX("mem heap used KB", (unsigned)mi.uordblks / 1024);
        PortDbgX("mem heap size KB", (unsigned)(fake_heap_end - fake_heap_start) / 1024);
    }
    PortDbgX("perf draws/frame", gPortPerfDraws / frames);
    { /* citro3d state calls per frame (each marks state citro3d re-sends with the next draw) */
        extern u32 gPortC3dCalls[6];
        static const char* const n[6] = { "perf c3d shader loads/frame", "perf c3d const sets/frame",
                                          "perf c3d tex binds same/frame", "perf c3d tex binds new/frame",
                                          "perf c3d sampler sets/frame", "perf c3d tex uploads/frame" };
        int q;
        for (q = 0; q < 6; q++) { PortDbgX(n[q], gPortC3dCalls[q] / frames); gPortC3dCalls[q] = 0; }
    }
    {
        extern u32 gPortPerfMemQueries, gPortPerfMemHits, gPortPerfDlCmds, gPortPerfDlCalls;
        PortDbgX("perf mem queries/frame (svc)", gPortPerfMemQueries / frames);
        PortDbgX("perf mem query cache hits/frame", gPortPerfMemHits / frames);
        PortDbgX("perf dl commands/frame", gPortPerfDlCmds / frames);
        PortDbgX("perf dl G_DL calls/frame", gPortPerfDlCalls / frames);
        gPortPerfMemQueries = gPortPerfMemHits = gPortPerfDlCmds = gPortPerfDlCalls = 0;
        { /* the 8 most frequent opcodes: opcode << 16 | count per frame */
            extern u32 gPortPerfOpCounts[256];
            int k, i;
            for (k = 0; k < 8; k++) {
                int best = -1;
                for (i = 0; i < 256; i++) if (best < 0 || gPortPerfOpCounts[i] > gPortPerfOpCounts[best]) best = i;
                if (gPortPerfOpCounts[best] == 0) break;
                PortDbgX("perf dl top opcode<<16|count", ((unsigned)best << 16) | (gPortPerfOpCounts[best] / frames));
                gPortPerfOpCounts[best] = 0;
            }
            memset(gPortPerfOpCounts, 0, sizeof(gPortPerfOpCounts));
        }
        { /* perf_stages: the 8 most expensive opcodes, opcode << 20 | us per frame */
            extern u64 gPortPerfOpTicks[256];
            extern int gPortPerfStagesOn;
            int k, i;
            for (k = 0; k < 8 && gPortPerfStagesOn; k++) {
                int best = 0;
                for (i = 1; i < 256; i++) if (gPortPerfOpTicks[i] > gPortPerfOpTicks[best]) best = i;
                if (gPortPerfOpTicks[best] == 0) break;
                PortDbgX("perf dl op<<20|us/frame", ((unsigned)best << 20) | (unsigned)(gPortPerfOpTicks[best] / div));
                gPortPerfOpTicks[best] = 0;
            }
            memset(gPortPerfOpTicks, 0, sizeof(gPortPerfOpTicks));
        }
    }
    if (sProfOn) { /* sampling profiler: per-mille of samples per stage */
        static const char* const names[PROF_COUNT] = {
            "prof game", "prof dl walk", "prof vtx", "prof tri setup", "prof tri build", "prof tri emit",
            "prof tex", "prof rect", "prof mtx", "prof flush", "prof audio", "prof pace", "prof swap",
            "prof gpu wait", "prof replay", "prof input", "prof vtx.light", "prof submit" };
        u32 tot = 0;
        int i;
        for (i = 0; i < PROF_COUNT; i++) tot += sProfHist[i];
        for (i = 0; i < PROF_COUNT && tot; i++) {
            PortDbgX(names[i], (unsigned)((u64)sProfHist[i] * 1000 / tot));
            sProfHist[i] = 0;
        }
    }
    sPerfGame = sPerfDl = sPerfSwap = sPerfPace = 0;
    gPortPerfTris = gPortPerfDraws = 0;
}

static double sWalkMs = 20.0;   /* measured: the recording walk + its shown frame, ms */
static double sReplayMs = 4.0;  /* measured: one replay frame, ms */
static int sReplayN;            /* in-between frames this logic frame */
static unsigned sReplayBrokenCnt;
static u64 sPerfInterp;             /* ticks in in-between passes */
static u32 sInterpFrames;           /* in-between frames drawn */
static double sInterpElapsedSum;    /* logic time before the passes, ms (summed per report) */
void PortGfx_RunTask(OSTask* task) {
    u64 tA = svcGetSystemTick(), tB, tC, tD;
    PROF_SET(PROF_INPUT);
    if (sPerfLastEnd != 0) sPerfGame += tA - sPerfLastEnd;
    if (!sGfxInited) PortGfx_Init();
    Port3ds_PollInput();
    { extern void Port3ds_PumpInput(void); Port3ds_PumpInput(); } /* live buttons -> game PadMgr */
    { extern void Audio_PortEnsureNullChannels(void); Audio_PortEnsureNullChannels(); } /* keep uninit audio channels non-NULL so direct game audio calls don't crash */
    if (sBench) {
        extern int gPortLegacyVbo;
        static unsigned sBenchTick;
        gPortLegacyVbo = (++sBenchTick) & 1;
    }
    if (!sInterp) { /* the tag bookkeeping still resets every frame (gfx_pc.c interp_group) */
        extern void gfx_interp_pass(float t, int record);
        gfx_interp_pass(1.0f, 0);
    }
    if (sInterp) {
        /* PORT (2026-09-30): 60 fps. The logic runs at 20/s (R_UPDATE_RATE retraces per update). The
         * display-list walk RECORDS the frame (gfx_pc.c "replay"), with every tagged matrix also evaluated
         * at t = 1/3 and 2/3 between the previous logic frame and this one (port_interp.h tags, the
         * Zelda64Recomp / Ship of Harkinian model); then each retrace shows a REPLAY - only the vertex
         * positions are recomputed and the recorded draws re-issued (a few ms instead of a whole walk).
         * As many in-between frames as fit (measured costs), so a slow scene/console drops to fewer and
         * the logic keeps its N64 speed. The world is shown one logic frame (50 ms) late, as in SoH. */
        extern void gfx_interp_begin_frame(void);
        extern void gfx_interp_pass(float t, int record);
        extern void gfx_replay_begin(void);
        extern int gPortReplayFirstK, Port3ds_InterpBlocked(void);
        const double kRetraceMs = 1000.0 / 59.83;
        int rate = Port3ds_UpdateRate();
        gfx_interp_begin_frame();
        gfx_interp_pass(1.0f, 1);
        sReplayN = 0;
        if (sLast != 0 && rate > 1 && !sBench && !Port3ds_InterpBlocked()) {
            double budget = rate * kRetraceMs, elapsed = (double)(osGetTime() - sLast);
            for (sReplayN = rate - 1; sReplayN > 0 && sInterp < 2; sReplayN--) {
                if (elapsed + sWalkMs + sReplayN * sReplayMs <= budget) break;
            }
            if (sReplayN > 2) sReplayN = 2; /* t = 1/3, 2/3 */
            sInterpElapsedSum += elapsed;
            gfx_replay_begin();
            gPortReplayFirstK = 2 - sReplayN; /* 2 extra: 1/3 first; 1: 2/3 first; 0: the logic frame */
        }
    }
    gfx_start_frame();
    gfx_run((Gfx*)task->t.data_ptr);
    tB = svcGetSystemTick();
    if (sBench) {
        extern int gPortLegacyVbo;
        sBenchDl[gPortLegacyVbo] += tB - tA;
        sBenchFrames[gPortLegacyVbo]++;
    }
    gfx_end_frame();
    tC = svcGetSystemTick();
    if (sInterp) {
        extern int gPortReplayBroken;
        extern void gfx_replay_frame(int k);
        const double kRetraceMs = 1000.0 / 59.83;
        int rate = Port3ds_UpdateRate(), i;
        double ms = (double)(tC - tA) / (SYSCLOCK_ARM11 / 1000.0);
        sWalkMs += (ms - sWalkMs) * 0.25;
        if (sReplayN > 0 && gPortReplayBroken) {
            sReplayN = 0; /* drawn directly (an off-screen render, a full buffer): no in-between frames */
            sReplayBrokenCnt++;
        }
        for (i = 0; i < sReplayN; i++) {
            u64 t0;
            PROF_SET(PROF_PACE);
            Port3ds_WaitUntil((double)sLast + (rate * kRetraceMs) * (i + 1) / (sReplayN + 1));
            t0 = svcGetSystemTick();
            gfx_replay_frame(2 - sReplayN + 1 + i);
            gfx_end_frame();
            ms = (double)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0);
            sReplayMs += (ms - sReplayMs) * 0.25;
            sPerfInterp += svcGetSystemTick() - t0;
            sInterpFrames++;
        }
        tC = svcGetSystemTick();
    }
    PROF_SET(PROF_PACE);
    Port3ds_PaceFrame();
    PROF_SET(PROF_GAME);
    tD = svcGetSystemTick();
    sPerfDl += tB - tA;
    sPerfSwap += tC - tB;
    sPerfPace += tD - tC;
    sPerfLastEnd = tD;
    /* Frame-rate log: game updates per second measured on the wall clock (x10), every 300 frames.
     * OoT's logic is designed for 20/s (R_UPDATE_RATE=3 VI retraces per update at 60 Hz). */
    { static u64 t0 = 0; static unsigned n = 0;
      if (t0 == 0) t0 = osGetTime();
      if (++n == 300) { u64 t1 = osGetTime();
          extern void PortDbgX(const char*, unsigned); extern void* gRegEditor;
          PortDbgX("perf updates/s x10", (unsigned)(3000000ull / (t1 - t0 ? t1 - t0 : 1)));
          PortDbgX("perf frames shown/s x10 (60fps interp)",
                   (unsigned)((300ull + sInterpFrames) * 10000ull / (t1 - t0 ? t1 - t0 : 1)));
          { extern uint32_t gPortInterpMatched, gPortInterpMissed;
            PortDbgX("perf interp mtx matched/frame", sInterpFrames ? gPortInterpMatched / sInterpFrames : 0);
            PortDbgX("perf interp mtx missed/frame", sInterpFrames ? gPortInterpMissed / sInterpFrames : 0);
            PortDbgX("perf us/replay frame", sInterpFrames ? (unsigned)(sPerfInterp / sInterpFrames / (SYSCLOCK_ARM11 / 1000000)) : 0);
            PortDbgX("perf us/walk+first frame (est)", (unsigned)(sWalkMs * 1000.0));
            PortDbgX("perf us/replay frame (est)", (unsigned)(sReplayMs * 1000.0));
            PortDbgX("perf replay fallbacks (direct frames)", sReplayBrokenCnt);
            sReplayBrokenCnt = 0;
            PortDbgX("perf us/frame logic before passes", (unsigned)(sInterpElapsedSum * 1000.0 / n));
            sInterpElapsedSum = 0;
            { extern uint32_t gPortInterpSame, gPortInterpJump;
              PortDbgX("perf interp mtx identical/frame", sInterpFrames ? gPortInterpSame / sInterpFrames : 0);
              PortDbgX("perf interp mtx jump (not blended)/frame", sInterpFrames ? gPortInterpJump / sInterpFrames : 0);
              gPortInterpSame = gPortInterpJump = 0; }
            { extern uint32_t gPortInterpVtx, gPortInterpVtxMiss;
              PortDbgX("perf interp skin vtx blended/frame", sInterpFrames ? gPortInterpVtx / sInterpFrames : 0);
              PortDbgX("perf interp skin vtx loads unmatched/frame", sInterpFrames ? gPortInterpVtxMiss / sInterpFrames : 0);
              gPortInterpVtx = gPortInterpVtxMiss = 0; }
            gPortInterpMatched = gPortInterpMissed = 0; }
          sInterpFrames = 0; sPerfInterp = 0;
          if (gRegEditor) PortDbgX("perf R_UPDATE_RATE", (unsigned)*(short*)((char*)gRegEditor + 0x14 + 126 * 2));
          PortDbgX("perf audio pumps/s x10", (unsigned)((u64)sPortAudioPumps * 10000ull / (t1 - t0 ? t1 - t0 : 1)));
          sPortAudioPumps = 0;
          Port3ds_PerfReport(n);
          if (sBench && sBenchFrames[0] && sBenchFrames[1]) {
              PortDbgX("bench us/frame INDEXED", (unsigned)(sBenchDl[0] / sBenchFrames[0] / (SYSCLOCK_ARM11 / 1000000)));
              PortDbgX("bench us/frame ARRAY", (unsigned)(sBenchDl[1] / sBenchFrames[1] / (SYSCLOCK_ARM11 / 1000000)));
              sBenchDl[0] = sBenchDl[1] = 0;
              sBenchFrames[0] = sBenchFrames[1] = 0;
          }
          { /* perf_ab=1: alternate New 3DS speed / Old 3DS approximation every 4 reports (~1 min) so one
             * play session measures both (docs/3ds-60fps-plan.md P0) */
              extern int gPortPerfAB, gPortO3dsSim;
              static unsigned sReports;
              bool n3ds = false;
              APT_CheckNew3DS(&n3ds);
              PortDbgX(gPortO3dsSim ? "perf mode O3DS-sim (268MHz no L2)" : "perf mode N3DS (804MHz L2)", 1);
              if (gPortPerfAB && n3ds && (++sReports % 4) == 0) {
                  gPortO3dsSim = !gPortO3dsSim;
                  osSetSpeedupEnable(!gPortO3dsSim);
              }
          }
          n = 0; t0 = t1; } }
}

#define ROM_PATH "sdmc:/3ds/oot/baserom-decompressed.z64"
#define LOG_PATH "sdmc:/3ds/oot/boot.log"

static void boot_flush(void) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

/* PORT PERF (2026-09-18): logging must be CHEAP — it is called from engine paths that run
 * every frame. The old Log() did gspWaitForVBlank() (a ~16ms hardware wait) AND fopen/fclose
 * on the SD on EVERY call, so per-frame logging stalled the game to ~1fps (the roadmap's
 * "never ship per-frame logging" trap). Now: keep one file handle open, fwrite+fflush the
 * line (crash still leaves the last line on the card), and print to the console — no vblank
 * wait, no reopen. The bottom-screen console updates on the graph loop's own swap. */
static FILE* sLogFile = NULL;
static void Log(const char* s) {
    /* PORT (2026-09-20): an engine path calls the logger with an empty string ~15x/frame,
     * which flooded boot.log with bare '\n' (millions of lines / ~18 MB per session — real
     * SD-write load on hardware, the roadmap's "never ship per-frame logging" trap). Empty
     * lines carry no information, so drop them here at the chokepoint. */
    if (s == NULL || s[0] == '\0') return;
    if (!sTouchUi) printf("%s\n", s); /* the bottom screen is the touch panel once it is up */
    if (!sLogFile) sLogFile = fopen(LOG_PATH, "a");
    if (sLogFile) { fputs(s, sLogFile); fputc('\n', sLogFile); fflush(sLogFile); }
}

/* checkpoint logger called from engine init (main.c) */
void PortDbg(const char* s) { Log(s); }

/* hex value logger for diagnostics (e.g. scene-data dump) */
void PortDbgX(const char* label, unsigned val) {
    /* PORT (2026-09-21): format hex MANUALLY — sprintf("%s=%08x") is broken in the port's
     * libc (produces an empty buffer, which Log()'s empty-string guard then drops), so every
     * PortDbgX diagnostic was silently invisible. Manual formatting fixes all value-logging. */
    char buf[128]; int n = 0; int i;
    static const char hx[] = "0123456789abcdef";
    if (label) { while (label[n] != '\0' && n < 100) { buf[n] = label[n]; n++; } }
    buf[n++] = '='; buf[n++] = '0'; buf[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) buf[n++] = hx[(val >> i) & 0xF];
    buf[n] = '\0';
    Log(buf);
}

/* Fast file-only loggers for high-volume renderer tracing (no console print). */
void PortLogFast(const char* s) {
    if (s == NULL || s[0] == '\0') return;
    if (!sLogFile) sLogFile = fopen(LOG_PATH, "a");
    if (sLogFile) { fputs(s, sLogFile); fputc('\n', sLogFile); }
}
void PortLogFastX(const char* label, unsigned val) {
    char buf[96];
    sprintf(buf, "%s=%08x", label, val);
    PortLogFast(buf);
}

/* Return the end address (base+size) of the mapped, readable memory block that
 * contains `addr`, or 0 if `addr` is unmapped/unreadable. The display-list
 * interpreter uses this to bound its reads: an un-terminated or garbage DL that
 * would otherwise walk into unmapped memory and data-abort is stopped cleanly at
 * the edge of its mapped block. One svcQueryMemory per 4KB page walked = cheap. */
/* PORT PERF (2026-09-30): the display-list walker and texture imports check every new memory block
 * with svcQueryMemory (a kernel call). The app's memory map is fixed after boot (heaps are reserved up
 * front), so readable ranges already confirmed are remembered: 8 most recent, round-robin. */
u32 gPortPerfMemQueries, gPortPerfMemHits;
u64 gPortPerfAudOpTicks[32]; /* audio_microcode.c: ticks per microcode op (perf_stages) */
unsigned PortMem_ReadableEnd(unsigned addr) {
    static unsigned sBase[8], sEnd[8];
    static int sNext;
    MemInfo mi;
    PageInfo pi;
    int i;
    for (i = 0; i < 8; i++) {
        if (addr - sBase[i] < sEnd[i] - sBase[i]) {
            gPortPerfMemHits++;
            return sEnd[i];
        }
    }
    gPortPerfMemQueries++;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return 0;
    if (mi.state == MEMSTATE_FREE || mi.state == MEMSTATE_RESERVED) return 0;
    if (!(mi.perm & MEMPERM_READ)) return 0;
    sBase[sNext] = mi.base_addr;
    sEnd[sNext] = mi.base_addr + mi.size;
    sNext = (sNext + 1) & 7;
    return mi.base_addr + mi.size;
}

/* Keep the bottom-screen console up so the message is readable instead of
 * silently bouncing back to the HOME menu. */
static void boot_halt(const char* msg) {
    Log(msg);
    printf("Press START to exit.\n");
    boot_flush();
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

/* ===== MINIMAL BOOT TEST (disabled) =====
 * Isolation build used to prove the CIA packaging. Kept for future debugging.
 * Renamed out of the way; the real entry point is main() below. */
int main_minimal(int argc, char** argv) {
    (void)argc; (void)argv;
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    int n = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        printf("\x1b[2;2HOoT port MINIMAL boot test -- frame %d   ", n++);
        printf("\x1b[4;2HIf you can read this, packaging is OK.");
        printf("\x1b[6;2HPress START to exit.");
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}

/* Delete any existing Luma crash dumps at boot. Luma writes crash_dump_%08u.dmp
 * using the lowest free index, so with the folder emptied each launch, this
 * session's crash (if any) always lands as crash_dump_00000000.dmp — one file,
 * same name, which makes the user's Mac auto-transfer trivial. */
static void WipeCrashDumps(void) {
    const char* dir = "sdmc:/luma/dumps/arm11";
    DIR* d = opendir(dir);
    if (d == NULL) return;
    struct dirent* ent;
    char path[300];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        remove(path);
    }
    closedir(d);
}

/* full boot path — the real entry point */
/* PORT DEBUG: svcOutputDebugString markers show up in Azahar's log with timestamps —
 * reliable boot-progress tracing before the SD boot.log is even open. */
#define DBG(s) svcOutputDebugString((s), sizeof(s) - 1)

/* PORT (2026-09-29): first thing in main, before any engine code: prove the process started and record
 * how it was launched and what memory it got (a hardware CIA launch showed nothing at all). */
static void Port_EarlyBootMarker(void) {
    FILE* f = fopen("sdmc:/3ds/oot/boot_early.log", "a"); /* own file: boot.log is recreated later */
    if (f != NULL) {
        bool isNew = false;
        u64 programId = 0;
        APT_CheckNew3DS(&isNew);
        APT_GetProgramID(&programId);
        fprintf(f, "=== main() reached: program %016llx, %s launch, %s 3DS, app mem %lu KB (free %lu KB), linear free %lu KB ===\n",
                (unsigned long long)programId, envIsHomebrew() ? "3dsx" : "CIA/3ds", isNew ? "New" : "Old",
                (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024),
                (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
                (unsigned long)(linearSpaceFree() / 1024));
        fclose(f);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Port_EarlyBootMarker();
    { extern void PortOverlayStatics_Init(void); PortOverlayStatics_Init(); } /* before any game code */
    DBG("PORT: main() entered");
    /* PORT PERF (2026-09-28): New 3DS: run the app core at 804 MHz with the L2 cache (default is the
     * original 268 MHz mode, ~3x slower). No effect on an original 3DS. */
    osSetSpeedupEnable(true);
    gfxInitDefault();
    DBG("PORT: gfxInitDefault done");
    consoleInit(GFX_BOTTOM, NULL);
    DBG("PORT: consoleInit done");
    { extern void PortCompat_InitStreams(void); PortCompat_InitStreams(); }
    WipeCrashDumps(); /* keep only this run's crash dump, named crash_dump_00000000.dmp */

    /* Truncate the log file at the start of every boot. */
    { FILE* f = fopen(LOG_PATH, "w");
      if (f) { fputs("=== OoT 3DS boot log ===\n", f); fclose(f); } }

    Log("OoT 3DS-Port booting...");
    /* ndsp plumbing AFTER the log is set up so its init status is visible (was before the
     * truncation above, which wiped its logs). Tries HLE even without dspfirm.cdc. */
    { extern void Port3ds_AudioInit(void); Port3ds_AudioInit(); }

    /* PORT DEBUG: the port assumes the linear heap is at 0x08000000 (segment-8 collision
     * handling). Log where libctru's linear heap actually lands on this Azahar/firmware. */
    { void* _lp = linearAlloc(0x1000);
      void* _lp2 = linearAlloc(0x100000);
      char _b[128];
      snprintf(_b, sizeof(_b), "MEM: linear base=%08x  +1MB=%08x  (24MB set)",
               (unsigned)(uintptr_t)_lp, (unsigned)(uintptr_t)_lp2);
      Log(_b);
      if (_lp) linearFree(_lp); if (_lp2) linearFree(_lp2); }

    FILE* rf = fopen(ROM_PATH, "rb");
    if (rf == NULL) { boot_halt("ROM not found at " ROM_PATH); gfxExit(); return 0; }
    fclose(rf);
    Log("ROM found.");

    PortDma_Init(ROM_PATH);
    Port3ds_LoadSettings();
    Port3ds_ProfStart();
    {
        extern int gPortO3dsSim;
        if (gPortO3dsSim) {
            osSetSpeedupEnable(false);
            DBG("PORT: o3ds_sim: New 3DS speedup OFF (268 MHz, no L2) - Old 3DS approximation");
        }
    }

    Log("DMA init OK.");
    /* PORT (2026-09-24): bootproc() normally calls Locale_Init (cart header -> gCurrentRegion,
     * which SaveContext_Init turns into the save language). The port enters Main() directly,
     * so region stayed 0 and the US ROM showed Japanese text. Run it here, after the ROM opens. */
    { extern void Locale_Init(void); extern int gCurrentRegion;
      Locale_Init(); PortDbgX("region (1=JP 2=US 3=EU)", (unsigned)gCurrentRegion); }

    gViConfigModeType = 0;

    Log("calling Main() (engine init)...");
    Main(0);
    Log("Main() returned; entering graph loop.");

    /* Audio isn't initialized on 3DS (audio thread never runs), so the SFX bank
     * link-lists are garbage and any Audio_StopSfxById/etc. walk spins forever.
     * Audio_ResetSfx() is CPU-side only (resets gSfxBanks to empty) and makes
     * all the SFX functions safe until real audio lands. */
    { extern void Audio_ResetSfx(void); Audio_ResetSfx(); Log("Audio_ResetSfx (sfx banks) done"); }
    /* Point gAudioCtx table pointers at the native compiled tables so direct game
     * reads (e.g. fanfare -> AudioLoad_GetFontsForSequence) can't NULL-deref. */
    { extern void Audio_PortInitTables(void); Audio_PortInitTables(); Log("Audio_PortInitTables done"); }
    /* Real audio bring-up: Audio_Init (AudioLoad_Init) sets up the audio heap and
     * loads the spec, which sets audioBufferParameters.specUnk4 (nonzero). Without
     * it AudioThread_Update's task path is gated off (specUnk4==0) so no synthesis
     * task is ever built. DMA handler defaults to osEPiStartDma (port-routed). */
    { extern void Audio_Init(void); extern void Audio_InitSound(void);
      Audio_Init(); Log("Audio_Init done");
      Audio_InitSound(); Log("Audio_InitSound done"); }

    Port3ds_TouchUiInit(); /* boot finished: the bottom screen becomes the control panel */
    Graph_ThreadEntry(0);

    boot_halt("graph loop exited");
    gfxExit();
    return 0;
}

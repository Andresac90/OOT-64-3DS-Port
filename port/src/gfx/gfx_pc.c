#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>
#include "ultra64/gs2dex.h" /* uObjBg for S2DEX background rectangles */

#include "config.h"

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"

uintptr_t gfx_port_segments[16];
unsigned int gfx_port_tri_count;
unsigned int gfx_port_frame_index;
extern uintptr_t gSegments[];

#ifdef __3DS__
/* Log helpers (file-only, no vsync) used by the DL-walk crash guard below. */
extern void PortLogFast(const char* s);
extern void PortLogFastX(const char* label, unsigned val);
/* Top of the loaded binary image (3dsx linker symbol); bounds the native
 * static-image pointer range in seg_addr. */
extern char __end__[];
#endif

/* PORT: warn-once instead of abort so renderer limits don't crash */
#define SUPPORT_CHECK(x) do { static int _w=0; if(!(x)&&!_w){_w=1; fprintf(stderr, "[gfx] unsupported: %s\n", #x);} } while(0)

/* PORT: texel/TLUT byte access, endian-corrected.
 * Texture and palette data are stored as u64[] blobs. On the N64 (big-endian)
 * the 8 bytes of each u64 are in memory order; our native little-endian build
 * reverses them within each u64, so the byte at logical offset k physically
 * lives at k^7 (base is u64-aligned). Every raw texel/palette read below indexes
 * through this to recover N64 byte order. Geometry (Vtx/Gfx) is unaffected — it's
 * typed struct fields the compiler already lays out correctly for LE. */
#ifdef __3DS__
/* PORT (2026-09-24): texel reads (TEXB) now come from the logical-order staging buffer built by
 * gfx_gather_texture(); palette reads (TEXP) still read memory in place, so they keep the
 * per-8-byte swizzle of natively-compiled (u64-array) assets. */
#define TEXB(base, k) ((base)[(k)])
#define TEXP(base, k) ((base)[(uintptr_t)(k) ^ 7u])
#define TEX_SRC_BYTE(p) (*(const uint8_t*)((uintptr_t)(p) ^ 7u)) /* absolute-address unswizzle */
#define TEX_SRC_BYTE_X(p, x) (*(const uint8_t*)((uintptr_t)(p) ^ (x)))
#else
#define TEX_SRC_BYTE_X(p, x) (*(const uint8_t*)(p))
#define TEXB(base, k) ((base)[(k)])
#define TEXP(base, k) ((base)[(k)])
#define TEX_SRC_BYTE(p) (*(const uint8_t*)(p))
#endif

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

/* 4:3 content is centered in the target: N64 x -> x * RATIO_Y + GFX_PILLAR_X */
#define GFX_PILLAR_X ((gfx_current_dimensions.width - SCREEN_WIDTH * RATIO_Y) / 2.0f)

#define MAX_BUFFERED 256
/* PORT (2026-09-25): F3DEX2 supports 7 directional lights + ambient (G_MAX_LIGHTS); sm64 used 2. OoT
 * sends up to 7 (e.g. torch-lit rooms): with 2, G_MW_NUMLIGHT made the lighting loop write
 * current_lights_coeffs past the array, which corrupted current_num_lights and then everything after
 * rsp (buf_vbo_len / buf_vbo_num_tris -> out-of-bounds vertex reads, found by the tools/statediff
 * scene tour in the Treasure Chest Shop), and lights 3..7 were dropped. */
#define MAX_LIGHTS 7
#define MAX_VERTICES 64

struct RGBA {
    uint8_t r, g, b, a;
};

struct XYWidthHeight {
    uint16_t x, y, width, height;
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t clip_rej;
};

struct TextureHashmapNode {
    struct TextureHashmapNode *next;
    
    const uint8_t *texture_addr;
    uint8_t fmt, siz;
    uint32_t size_bytes; /* PORT: part of the key -- tile loads can share a start address */
    uint32_t pal_hash;   /* PORT: CI textures: hash of the palette colors they use (0 otherwise) */
    uint32_t line_size_bytes; /* PORT: the tile's row pitch: same bytes, different width = different texture */
    uint16_t width, height;   /* PORT: dimensions uploaded (texture coordinates are normalized to them) */
    
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
};
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;


static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;
    
    float MP_matrix[4][4];
    float P_matrix[4][4];
    
    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    uint8_t clip_ratio; /* G_MW_CLIP guard band (FRUSTRATIO_n) */
    float half_px_x, half_px_y; /* half an N64 pixel of the current viewport, in NDC */
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;
    
    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];
} rsp;

static struct RDP {
    const uint8_t *palette;
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint32_t width; /* texture image width in texels (G_SETTIMG), needed by LOADTILE */
    } texture_to_load;
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
        uint32_t line_size_bytes;            /* bytes loaded per row */
        uint32_t full_image_line_size_bytes; /* source row stride (== line for LOADBLOCK) */
    } loaded_texture[2];
    /* PORT (2026-09-25): the 8 RDP tile descriptors (was: only the render tile). Texture unit i draws
     * with tile first_tile + i (G_TEXTURE's tile), whose TMEM slot is tmem != 0 (libultraship's model:
     * one texture at TMEM 0, another anywhere else). TEXEL1 had no tile of its own before. */
    struct TileDesc {
        uint8_t fmt;
        uint8_t siz;
        uint8_t palette; /* CI4 palette bank (G_SETTILE palette field) */
        uint8_t cms, cmt;
        uint8_t shifts, shiftt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
        uint16_t tmem;
    } tiles[8];
    uint8_t first_tile;
    bool drawing_rect; /* texture rectangles take no bilinear half-texel offset */
    bool textures_changed[2];
    /* PORT (2026-09-25): TLUT memory like the RDP's upper TMEM: 256 colors (logical big-endian RGBA5551),
     * loaded by G_LOADTLUT at (load tile tmem - 256); CI4 reads bank tile.palette*16, CI8 all 256.
     * Before, "the last TLUT's address" was used for every CI texture (bank ignored) - tools/statediff
     * fbdiff attributed most wrong pixels in Kokiri Forest / Fire Temple to CI draws. */
    uint16_t tlut[256];
    
    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode; /* raw G_SETCOMBINE: (w0 & 0xFFFFFF) << 32 | w1, decoded per draw (gfx_cc_key) */
    uint8_t prim_lod_frac;

    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
    uint32_t color_image_width;
    float vp_raw[4];          /* N64 viewport: x, y (bottom origin), width, height, in N64 pixels */
    uint32_t scissor_raw[4];  /* G_SETSCISSOR ulx, uly, lrx, lry (10.2) */
} rdp;

static struct RenderingState {
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    uint64_t cc_id0; /* combiner key of shader_program (gfx_cc.h) */
    uint32_t cc_id1;
    struct GfxCombineConsts consts; /* last constants handed to the backend */
    struct TextureHashmapNode *textures[2];
} rendering_state;

struct GfxDimensions gfx_current_dimensions;

static bool dropped_frame;

static float buf_vbo[MAX_BUFFERED * (26 * 3)]; // 3 vertices in a triangle and 26 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

#include <time.h>
static unsigned long get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* PORT PERF (2026-09-28): stage timers (opt-in, see PORT_PERF_STAGES) for the frame profiler (3ds_main.c Port3ds_PerfReport), in
 * 268 MHz ticks: texture import, vertex transform, triangle setup (excluding flushes), backend draws. */
#ifdef __3DS__
#include <3ds/svc.h>
u64 gPortPerfTex, gPortPerfVtx, gPortPerfTri, gPortPerfFlush, gPortPerfEmit, gPortPerfMtx;
u32 gPortPerfTexImports;
#ifdef PORT_PERF_STAGES /* opt-in (PORT_EXTRA=-DPORT_PERF_STAGES): one system call per triangle */
#define PERF_T() svcGetSystemTick()
#else
#define PERF_T() 0
#endif
#else
#define PERF_T() 0
static uint64_t gPortPerfTex, gPortPerfVtx, gPortPerfTri, gPortPerfFlush, gPortPerfEmit, gPortPerfMtx;
static uint32_t gPortPerfTexImports;
#endif
static void gfx_flush_impl(void);
static void import_texture_impl(int unit, int tile_index);
static void gfx_flush(void) {
    uint64_t t0 = PERF_T();
    gfx_flush_impl();
    gPortPerfFlush += PERF_T() - t0;
}
static void import_texture(int unit, int tile_index) {
    uint64_t t0 = PERF_T();
    import_texture_impl(unit, tile_index);
    gPortPerfTex += PERF_T() - t0;
    gPortPerfTexImports++;
}

#ifdef __3DS__
/* PORT (2026-09-24): minimal render-target routing. The citro3d backend has ONE target (the
 * screen), but OoT renders into other color images too: the pause menu's Link preview
 * (Player_DrawPause -> its own color/depth buffers), PreRender framebuffer save/copy/filter
 * passes, transition tiles. Painting those onto the screen covered the pause menu with
 * their full-rect clears. Only draws whose color image is a real framebuffer (SysCfb) reach
 * the screen; other targets are dropped until real render-to-texture exists (roadmap G10). */
extern uintptr_t sSysCfbFbPtr[2];
static inline int gfx_cimg_is_screen(void) {
    uintptr_t a = (uintptr_t)rdp.color_image_address;
    /* a framebuffer address with another width is a different image (Player_DrawPause's 64-wide one) */
    return a == 0 || ((a == sSysCfbFbPtr[0] || a == sSysCfbFbPtr[1]) &&
                      (rdp.color_image_width == 0 || rdp.color_image_width == SCREEN_WIDTH));
}
#else
static inline int gfx_cimg_is_screen(void) { return 1; }
#endif

#ifdef __3DS__
/* Per-draw attribution for tools/statediff fbdiff (renderer vs ares): while the color readback runs,
 * every flush (one render state) gets an id 1..254 that the backend writes into the stencil buffer,
 * and its state is logged here. A differing pixel then names the draw - and the GBI state - that
 * produced it. Logs are kept per finished frame, aligned with gfx_3ds.c's color/depth slots. */
typedef struct {
    uint16_t id, tris;
    uint64_t cc; /* combiner key (gfx_cc.h) */
    uint32_t omh, oml, geo, prim, env;
    const void* tex0;
    const void* tex1;
    uint8_t fmt, siz, blend, cimg_screen, pal;
    uint16_t tw, th;
    const void* tlutsrc;
    uint16_t tlut0, tlut1; /* first two colors of the bank the draw uses (CI) */
    uint32_t fogcol;
    int16_t fogmul, fogoff;
    float v0[10]; /* first packed vertex: pos(4) [uv(2)] then fog / color floats */
    const void* cmd; /* DL command (address) that produced the last geometry of the batch */
    uint16_t nsub, nbehind; /* source triangles subdivided / clipped for a vertex behind the eye */
} PortDrawRec;
static uint16_t sBatchSub, sBatchBehind;
#define PORT_DRAWLOG_MAX 2048
static PortDrawRec sDrawLogCur[PORT_DRAWLOG_MAX], sDrawLog[2][PORT_DRAWLOG_MAX];
static const Gfx* sPortCurCmd;   /* command being interpreted */
static int sDrawLogCurN, sDrawLogN[2], sDrawSeq;
extern int Port3ds_DrawIdActive(void);
extern int Port3ds_ColorLatestSlot(void);
extern void gfx_citro3d_set_draw_id(int id);

static PortDrawRec sPendingRec; /* state of the batch being built, captured at its first triangle */

/* called for every triangle while buffering: the first triangle of a batch snapshots the state the
 * whole batch is drawn with (rdp can change before the batch is flushed, so flush time is too late) */
static void port_draw_snapshot(void) {
    PortDrawRec* r = &sPendingRec;
    if (buf_vbo_num_tris != 0 || !Port3ds_DrawIdActive()) {
        return;
    }
    r->cc = rendering_state.cc_id0;
    r->omh = rdp.other_mode_h;
    r->oml = rdp.other_mode_l;
    r->geo = rsp.geometry_mode;
    r->prim = (rdp.prim_color.r << 24) | (rdp.prim_color.g << 16) | (rdp.prim_color.b << 8) | rdp.prim_color.a;
    r->env = (rdp.env_color.r << 24) | (rdp.env_color.g << 16) | (rdp.env_color.b << 8) | rdp.env_color.a;
    r->tex0 = rdp.loaded_texture[0].addr;
    r->tex1 = rdp.loaded_texture[1].addr;
    const struct TileDesc* rt = &rdp.tiles[rdp.first_tile];
    r->fmt = rt->fmt;
    r->siz = rt->siz;
    r->tw = (rt->lrs - rt->uls + 4) / 4;
    r->th = (rt->lrt - rt->ult + 4) / 4;
    r->blend = rendering_state.alpha_blend;
    r->cimg_screen = gfx_cimg_is_screen();
    r->pal = rt->palette;
    r->tlutsrc = rdp.palette;
    r->tlut0 = rdp.tlut[((rt->siz == G_IM_SIZ_4b) ? (rt->palette & 15) * 16 : 0)];
    r->tlut1 = rdp.tlut[((rt->siz == G_IM_SIZ_4b) ? (rt->palette & 15) * 16 : 0) + 1];
    r->fogcol = (rdp.fog_color.r << 24) | (rdp.fog_color.g << 16) | (rdp.fog_color.b << 8) | rdp.fog_color.a;
    r->fogmul = rsp.fog_mul;
    r->fogoff = rsp.fog_offset;
    r->cmd = sPortCurCmd;
}

static void port_draw_id_begin(void) {
    PortDrawRec* r;
    if (!Port3ds_DrawIdActive()) {
        gfx_citro3d_set_draw_id(0);
        return;
    }
    r = (sDrawLogCurN < PORT_DRAWLOG_MAX) ? &sDrawLogCur[sDrawLogCurN++] : NULL;
    sDrawSeq++;
    gfx_citro3d_set_draw_id((sDrawSeq - 1) % 254 + 1);
    if (r != NULL) {
        *r = sPendingRec;
        r->id = (sDrawSeq - 1) % 254 + 1;
        r->tris = buf_vbo_num_tris;
        r->nsub = sBatchSub;
        r->nbehind = sBatchBehind;
        memcpy(r->v0, buf_vbo, sizeof(r->v0));
    }
}

static void port_draw_log_commit(void) { /* after the frame's readbacks: log -> that frame's slot */
    int slot = Port3ds_ColorLatestSlot();
    if (slot >= 0 && Port3ds_DrawIdActive()) {
        memcpy(sDrawLog[slot], sDrawLogCur, sizeof(PortDrawRec) * sDrawLogCurN);
        sDrawLogN[slot] = sDrawLogCurN;
    }
    sDrawLogCurN = 0;
    sDrawSeq = 0;
}

/* back = 0: most recently finished frame (same slot convention as Port3ds_GetColor) */
void PortGfx_WriteDrawLog(int back, const char* path) {
    int slot = Port3ds_ColorLatestSlot(), i;
    FILE* f;
    if (slot < 0 || (f = fopen(path, "w")) == NULL) {
        return;
    }
    if (back) {
        slot ^= 1;
    }
    fprintf(f, "# id tris cc omh oml geo prim env tex0 tex1 fmt siz tw th blend screen pal tlutsrc tlut0 tlut1 fogcol fogmul fogoff v0[0..9]\n");
    for (i = 0; i < sDrawLogN[slot]; i++) {
        const PortDrawRec* r = &sDrawLog[slot][i];
        fprintf(f, "%u %u %016llx %08lx %08lx %08lx %08lx %08lx %p %p %u %u %u %u %u %u %u %p %04x %04x\n", r->id,
                r->tris, (unsigned long long)r->cc, (unsigned long)r->omh, (unsigned long)r->oml, (unsigned long)r->geo,
                (unsigned long)r->prim, (unsigned long)r->env, r->tex0, r->tex1, r->fmt, r->siz, r->tw, r->th,
                r->blend, r->cimg_screen, r->pal, r->tlutsrc, r->tlut0, r->tlut1);
        fseek(f, -1, SEEK_CUR); /* append the fog fields to the same line */
        fprintf(f, " %08lx %d %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", (unsigned long)r->fogcol, r->fogmul,
                r->fogoff, r->v0[0], r->v0[1], r->v0[2], r->v0[3], r->v0[4], r->v0[5], r->v0[6], r->v0[7], r->v0[8],
                r->v0[9]);
        fseek(f, -1, SEEK_CUR);
        fprintf(f, " cmd=%p sub=%u behind=%u\n", r->cmd, r->nsub, r->nbehind);
    }
    fclose(f);
}
#endif

static void gfx_flush_impl(void) {
    if (buf_vbo_len > 0) {
        int num = buf_vbo_num_tris;
        unsigned long t0 = get_time();
#ifdef __3DS__
        port_draw_id_begin();
#endif
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
#ifdef __3DS__
        sBatchSub = sBatchBehind = 0;
#endif
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
        unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}

/* PORT (2026-09-25): combiner key (gfx_cc.h), decoded from the raw G_SETCOMBINE words per the RDP
 * mux tables (A,B: 4 bits, C: 5 bits, D: 3 bits for RGB; 3 bits each for alpha, where C has its own
 * table). Replaces the sm64 3-bit encoding (no ONE / COMBINED / alpha broadcasts, cycle 1 dropped). */
static uint8_t cc_rgb_src(uint32_t v, int slot) {
    switch (v) {
        case G_CCMUX_COMBINED: return CCS_COMB;
        case G_CCMUX_TEXEL0: return CCS_TEX0;
        case G_CCMUX_TEXEL1: return CCS_TEX1;
        case G_CCMUX_PRIMITIVE: return CCS_PRIM;
        case G_CCMUX_SHADE: return CCS_SHADE;
        case G_CCMUX_ENVIRONMENT: return CCS_ENV;
        case 6: return (slot == 0 || slot == 3) ? CCS_1 : CCS_0; /* B: key center, C: key scale */
        case 7: return slot == 2 ? CCS_COMBA : CCS_0;          /* A: noise, B: K4, D: zero */
    }
    if (slot == 2) {
        switch (v) {
            case G_CCMUX_TEXEL0_ALPHA: return CCS_TEX0A;
            case G_CCMUX_TEXEL1_ALPHA: return CCS_TEX1A;
            case G_CCMUX_PRIMITIVE_ALPHA: return CCS_PRIMA;
            case G_CCMUX_SHADE_ALPHA: return CCS_SHADEA;
            case G_CCMUX_ENV_ALPHA: return CCS_ENVA;
            case G_CCMUX_LOD_FRACTION: return CCS_LODF;
            case G_CCMUX_PRIM_LOD_FRAC: return CCS_PRIMLODF;
        }
    }
    return CCS_0;
}

static uint8_t cc_alpha_src(uint32_t v, int slot) {
    switch (v) {
        case 0: return slot == 2 ? CCS_LODF : CCS_COMB;
        case G_ACMUX_TEXEL0: return CCS_TEX0;
        case G_ACMUX_TEXEL1: return CCS_TEX1;
        case G_ACMUX_PRIMITIVE: return CCS_PRIM;
        case G_ACMUX_SHADE: return CCS_SHADE;
        case G_ACMUX_ENVIRONMENT: return CCS_ENV;
        case 6: return slot == 2 ? CCS_PRIMLODF : CCS_1;
    }
    return CCS_0;
}

static bool cc_refs(const uint8_t s[4], uint8_t a, uint8_t b) {
    for (int i = 0; i < 4; i++) {
        if (s[i] == a || s[i] == b) {
            return true;
        }
    }
    return false;
}

static uint64_t gfx_cc_key(bool two_cycle, bool use_alpha) {
    uint32_t w0 = (uint32_t)(rdp.combine_mode >> 32), w1 = (uint32_t)rdp.combine_mode;
    const uint32_t m[2][2][4] = {
        { { (w0 >> 20) & 0xF, (w1 >> 28) & 0xF, (w0 >> 15) & 0x1F, (w1 >> 15) & 7 },
          { (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7 } },
        { { (w0 >> 5) & 0xF, (w1 >> 24) & 0xF, w0 & 0x1F, (w1 >> 6) & 7 },
          { (w1 >> 21) & 7, (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7 } },
    };
    uint8_t s[2][2][4];
    for (int c = 0; c < 2; c++) {
        for (int ch = 0; ch < 2; ch++) {
            uint8_t* t = s[c][ch];
            for (int j = 0; j < 4; j++) {
                t[j] = ch ? cc_alpha_src(m[c][ch][j], j) : cc_rgb_src(m[c][ch][j], j);
                if (!two_cycle) { /* 1-cycle: TEXEL1 reads the same texel as TEXEL0 (libultraship) */
                    t[j] = t[j] == CCS_TEX1 ? CCS_TEX0 : t[j] == CCS_TEX1A ? CCS_TEX0A : t[j];
                } else if (c == 1) { /* the 2nd cycle sees the next tile: TEXEL0 <-> TEXEL1 */
                    static const uint8_t sw[16] = { 0, 1, CCS_TEX1, CCS_TEX0, CCS_TEX1A, CCS_TEX0A, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
                    t[j] = sw[t[j]];
                } else if (t[j] == CCS_COMB || t[j] == CCS_COMBA) {
                    t[j] = CCS_0; /* cycle 0 COMBINED: previous pixel's result, unused by OoT */
                }
            }
            if (t[0] == t[1] || t[2] == CCS_0) { /* (A-A)*C+D, (A-B)*0+D -> D */
                t[0] = t[1] = t[2] = CCS_0;
            }
        }
    }
    if (!two_cycle) {
        for (int ch = 0; ch < 2; ch++) {
            s[1][ch][0] = s[1][ch][1] = s[1][ch][2] = CCS_0;
            s[1][ch][3] = CCS_COMB;
        }
    } else {
        if (!cc_refs(s[1][0], CCS_COMB, CCS_COMB)) { /* cycle-0 RGB unused */
            s[0][0][0] = s[0][0][1] = s[0][0][2] = s[0][0][3] = CCS_0;
        }
        if (!cc_refs(s[1][0], CCS_COMBA, CCS_COMBA) && !cc_refs(s[1][1], CCS_COMB, CCS_COMB)) { /* cycle-0 alpha */
            s[0][1][0] = s[0][1][1] = s[0][1][2] = s[0][1][3] = CCS_0;
        }
    }
    if (!use_alpha) { /* alpha unused (no blending, no alpha test): constant 1 */
        s[0][1][0] = s[0][1][1] = s[0][1][2] = CCS_0;
        s[0][1][3] = CCS_1;
        s[1][1][0] = s[1][1][1] = s[1][1][2] = CCS_0;
        s[1][1][3] = CCS_COMB;
    }
    uint64_t key = 0;
    for (int c = 0; c < 2; c++) {
        for (int ch = 0; ch < 2; ch++) {
            for (int j = 0; j < 4; j++) {
                key |= (uint64_t)s[c][ch][j] << (c * 32 + ch * 16 + j * 4);
            }
        }
    }
    return key;
}

/* which inputs a key reads: SHADER_OPT_TEX0/TEX1, and bits 8.. = 1 << CCS_* for the constants */
static uint32_t gfx_cc_usage(uint64_t key) {
    uint32_t u = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t v = (key >> (i * 4)) & 0xF;
        if (v == CCS_TEX0 || v == CCS_TEX0A) {
            u |= SHADER_OPT_TEX0;
        } else if (v == CCS_TEX1 || v == CCS_TEX1A) {
            u |= SHADER_OPT_TEX1;
        } else {
            u |= 1u << (8 + v);
        }
    }
    return u;
}

/* PORT (2026-09-24): textures are cached by source address, which assumes texture memory never
 * changes. DMA'd buffers are reused in place (e.g. the A/B/START action labels in doActionSegment):
 * after a new label was DMA'd to the same address the cache kept returning the old one ("Attack"
 * instead of "Decide" on the pause screen). dma_shim.c calls this for every DMA destination; any
 * cached texture starting inside the written range is dropped (sentinel address never matches). */
void gfx_texture_cache_invalidate_range(const void* start, uint32_t size) {
    const uint8_t* s = (const uint8_t*)start;
    const uint8_t* e = s + size;
    size_t i;
    for (i = 0; i < gfx_texture_cache.pool_pos; i++) {
        struct TextureHashmapNode* n = &gfx_texture_cache.pool[i];
        if (n->texture_addr >= s && n->texture_addr < e) {
            n->texture_addr = (const uint8_t*)1;
        }
    }
}

static uint32_t gfx_ci_palette_hash(uint32_t fmt, uint32_t siz);
static const struct TileDesc* sImpTile; /* tile of the texture being imported (import_texture) */
static struct TextureHashmapNode* sImpNode; /* its cache node */

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz, uint32_t size_bytes) {
    uint32_t pal_hash = gfx_ci_palette_hash(fmt, siz);
    size_t hash = (uintptr_t)orig_addr;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz && (*node)->size_bytes == size_bytes &&
            (*node)->pal_hash == pal_hash && (*node)->line_size_bytes == sImpTile->line_size_bytes) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            *n = *node;
            return true;
        }
        node = &(*node)->next;
    }
    if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        // Pool is full. We just invalidate everything and start over.
        gfx_texture_cache.pool_pos = 0;
#ifdef __3DS__
        {
            static int sWraps;
            extern void PortDbgX(const char* label, unsigned val);
            if (sWraps++ < 16) {
                PortDbgX("TEXCACHE wrap at frame", (unsigned)gfx_port_frame_index);
            }
        }
#endif
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->pal_hash = pal_hash;
    (*node)->line_size_bytes = sImpTile->line_size_bytes;
    (*node)->width = (*node)->height = 0;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    (*node)->size_bytes = size_bytes;
    *n = *node;
    return false;
}

//do not allocate this buffer on the stack, as some platforms
//may have limited stack space
static uint8_t rgba32_buf[65536] __attribute__((aligned(32)));

static void gfx_upload_texture(uint8_t* buf, uint32_t width, uint32_t height) {
    gfx_rapi->upload_texture(buf, width, height);
    sImpNode->width = width;
    sImpNode->height = height;
}

static void import_texture_rgba16(int tile) {
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint16_t col16 = (TEXB(rdp.loaded_texture[tile].addr, 2 * i) << 8) | TEXB(rdp.loaded_texture[tile].addr, 2 * i + 1);
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = sImpTile->line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;
    
    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia4(int tile) {
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = TEXB(rdp.loaded_texture[tile].addr, i / 2);
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part >> 1;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_3_8(r);
        rgba32_buf[4*i + 1] = SCALE_3_8(g);
        rgba32_buf[4*i + 2] = SCALE_3_8(b);
        rgba32_buf[4*i + 3] = alpha ? 255 : 0;
    }
    
    uint32_t width = sImpTile->line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;

    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia8(int tile) {
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = TEXB(rdp.loaded_texture[tile].addr, i) >> 4;
        uint8_t alpha = TEXB(rdp.loaded_texture[tile].addr, i) & 0xf;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = SCALE_4_8(alpha);
    }
    
    uint32_t width = sImpTile->line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;
    
    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia16(int tile) {
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint8_t intensity = TEXB(rdp.loaded_texture[tile].addr, 2 * i);
        uint8_t alpha = TEXB(rdp.loaded_texture[tile].addr, 2 * i + 1);
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = alpha;
    }
    
    uint32_t width = sImpTile->line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;
    
    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_ci4(int tile) {
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = TEXB(rdp.loaded_texture[tile].addr, i / 2);
        uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint16_t col16 = rdp.tlut[((sImpTile->palette & 15) * 16 + idx) & 0xFF]; /* bank + index */
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = sImpTile->line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;
    
    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_ci8(int tile) {
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t idx = TEXB(rdp.loaded_texture[tile].addr, i);
        uint16_t col16 = rdp.tlut[idx];
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = sImpTile->line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;
    
    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_i4(int tile) {
    /* uses module-static rgba32_buf (3DS stack is tiny; no 32KB stack local) */
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = TEXB(rdp.loaded_texture[tile].addr, i / 2);
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part << 4 | part;
        rgba32_buf[4*i + 0] = intensity;
        rgba32_buf[4*i + 1] = intensity;
        rgba32_buf[4*i + 2] = intensity;
        rgba32_buf[4*i + 3] = intensity;
    }
    uint32_t width = sImpTile->line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;
    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_i8(int tile) {
    /* uses module-static rgba32_buf (3DS stack is tiny; no 64KB stack local) */
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = TEXB(rdp.loaded_texture[tile].addr, i);
        rgba32_buf[4*i + 0] = intensity;
        rgba32_buf[4*i + 1] = intensity;
        rgba32_buf[4*i + 2] = intensity;
        rgba32_buf[4*i + 3] = intensity;
    }
    uint32_t width = sImpTile->line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / sImpTile->line_size_bytes;
    gfx_upload_texture(rgba32_buf, width, height);
}

static void import_texture_rgba32(int tile) {
    /* Reorder bytes (TEXB) into the staging buffer rather than uploading the u64
     * blob directly, so the LE 8-byte reversal is corrected like the other formats. */
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        rgba32_buf[i] = TEXB(rdp.loaded_texture[tile].addr, i);
    }
    uint32_t width = sImpTile->line_size_bytes / 2;
    uint32_t height = (rdp.loaded_texture[tile].size_bytes / 2) / sImpTile->line_size_bytes;
    gfx_upload_texture(rgba32_buf, width, height);
}

/* PORT (2026-09-29): the unswizzle for a texture/TLUT source. u64[] asset arrays and game-written RAM keep
 * logical byte k at address ^ 7; the few asset arrays the decomp emits as u32[] (Kokiri, other townsfolk:
 * textures only 4-byte aligned in the ROM) keep it at address ^ 3. Reading those with ^ 7 scrambled
 * their palettes and texels (striped hair, camouflage skin). Table: port/src_gen/u32_asset_ranges.c. */
#ifdef __3DS__
#include "port_u32_assets.h"
static uintptr_t gfx_src_swizzle(const void* p) {
    static const PortU32Asset* sSorted[256];
    static int sN = -1;
    uintptr_t a = (uintptr_t)p;
    int lo, hi;
    if (sN < 0) { /* sort the linked arrays by address once */
        int i, j;
        sN = 0;
        for (i = 0; i < gPortU32AssetCount && sN < 256; i++) {
            if (gPortU32Assets[i].addr != NULL) {
                const PortU32Asset* e = &gPortU32Assets[i];
                for (j = sN++; j > 0 && (uintptr_t)sSorted[j - 1]->addr > (uintptr_t)e->addr; j--) {
                    sSorted[j] = sSorted[j - 1];
                }
                sSorted[j] = e;
            }
        }
    }
    lo = 0;
    hi = sN - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uintptr_t b = (uintptr_t)sSorted[mid]->addr;
        if (a < b) {
            hi = mid - 1;
        } else if (a >= b + sSorted[mid]->size) {
            lo = mid + 1;
        } else {
            return 3u;
        }
    }
    return 7u;
}
#else
#define gfx_src_swizzle(p) 0u
#endif

/* PORT (2026-09-24): copy the loaded texture into a staging buffer laid out like TMEM (each loaded
 * row at the render tile's line stride), in logical byte order. Handles LOADTILE sub-rects (source
 * stride = full image width) and unswizzles natively-compiled assets by ABSOLUTE address, which is
 * byte-identical to the old base-relative ^7 for 8-aligned bases. Returns staged size, 0 on overflow. */
static uint8_t sTexStage[65536] __attribute__((aligned(32)));
static uint32_t gfx_gather_texture(int tile) {
    const uint8_t* src = rdp.loaded_texture[tile].addr;
    uint32_t size = rdp.loaded_texture[tile].size_bytes;
    uint32_t row = rdp.loaded_texture[tile].line_size_bytes;
    uint32_t full = rdp.loaded_texture[tile].full_image_line_size_bytes;
    uint32_t dst_stride, rows, x, y;
    uintptr_t sw = gfx_src_swizzle(src);
    if (row == 0 || full == 0 || row == full) { /* contiguous (LOADBLOCK) */
        if (size > sizeof(sTexStage)) return 0;
        for (x = 0; x < size; x++) sTexStage[x] = TEX_SRC_BYTE_X(src + x, sw);
        return size;
    }
    rows = size / row;
    dst_stride = sImpTile->line_size_bytes;
    if (dst_stride < row) dst_stride = row;
    if (rows * dst_stride > sizeof(sTexStage)) return 0;
    for (y = 0; y < rows; y++) {
        for (x = 0; x < row; x++) sTexStage[y * dst_stride + x] = TEX_SRC_BYTE_X(src + y * full + x, sw);
        for (; x < dst_stride; x++) sTexStage[y * dst_stride + x] = 0;
    }
    return rows * dst_stride;
}

#ifdef PORT_TEXDUMP
/* DEBUG TOOL (opt-in, build with PORT_EXTRA=-DPORT_TEXDUMP): write every uniquely-addressed
 * texture the port decodes (source address, N64 fmt/siz, decoded RGBA8) to
 * sdmc:/3ds/oot/texdump.bin, for diffing against the decomp's golden PNGs
 * (tools/texdiff.py). Record: "TEXD" u32 addr, u8 fmt, u8 siz, u16 0, u32 w, u32 h, w*h*4 RGBA. */
static void texdump_record(const uint8_t* addr, uint8_t fmt, uint8_t siz, uint32_t size_bytes, uint32_t line_bytes) {
    static FILE* f = NULL;
    static const uint8_t* seen[512];
    static int nseen = 0, dead = 0;
    uint32_t bits, w, h, hdr[4];
    int i;
    if (dead || line_bytes == 0) return;
    for (i = 0; i < nseen; i++) if (seen[i] == addr) return;
    if (nseen >= 512) { if (f) { fclose(f); f = NULL; } dead = 1; return; }
    seen[nseen++] = addr;
    bits = (siz == G_IM_SIZ_4b) ? 4 : (siz == G_IM_SIZ_8b) ? 8 : (siz == G_IM_SIZ_16b) ? 16 : 32;
    w = (siz == G_IM_SIZ_32b) ? line_bytes / 2 : line_bytes * 8 / bits; /* 32b: TMEM line counts 2 bytes/texel */
    h = (siz == G_IM_SIZ_32b) ? (size_bytes / 2) / line_bytes : size_bytes / line_bytes;
    if (w == 0 || h == 0 || w * h * 4 > sizeof(rgba32_buf)) return;
    if (f == NULL) { f = fopen("sdmc:/3ds/oot/texdump.bin", "wb"); if (f == NULL) { dead = 1; return; } }
    hdr[0] = 0x44584554u; /* "TEXD" */
    hdr[1] = (uint32_t)(uintptr_t)addr;
    hdr[2] = (uint32_t)fmt | ((uint32_t)siz << 8);
    hdr[3] = w;
    fwrite(hdr, 4, 4, f);
    fwrite(&h, 4, 1, f);
    fwrite(rgba32_buf, 1, w * h * 4, f);
    fflush(f);
}
#endif

static void import_texture_impl(int unit, int tile_index) {
    sImpTile = &rdp.tiles[tile_index];
    int tile = sImpTile->tmem != 0; /* TMEM slot: the importers below index loaded_texture[] with it */
    uint8_t fmt = sImpTile->fmt;
    uint8_t siz = sImpTile->siz;

    if (gfx_texture_cache_lookup(unit, &rendering_state.textures[unit], rdp.loaded_texture[tile].addr, fmt, siz, rdp.loaded_texture[tile].size_bytes)) {
        return;
    }
    sImpNode = rendering_state.textures[unit];
#ifdef __3DS__
    /* Crash-proof the texel read: a bad segment/asset pointer can leave a texture
     * address in unmapped memory (the gap above the loaded image). Reading it in
     * the import loop would data-abort. Verify the whole texture is inside a mapped
     * block; if not, skip the upload (the previously-bound texture stays) rather
     * than crash — same philosophy as the DL-walk guard. */
    { extern unsigned PortMem_ReadableEnd(unsigned addr);
      uintptr_t a = (uintptr_t)rdp.loaded_texture[tile].addr;
      uint32_t _row = rdp.loaded_texture[tile].line_size_bytes, _full = rdp.loaded_texture[tile].full_image_line_size_bytes;
      uint32_t _extent = (_row && _full && _row != _full)
          ? (rdp.loaded_texture[tile].size_bytes / _row - 1) * _full + _row
          : rdp.loaded_texture[tile].size_bytes;
      unsigned e = PortMem_ReadableEnd((unsigned)a);
      if (e == 0 || a + _extent > (uintptr_t)e) {
          PortLogFastX("[TEX] skip unmapped tex", (unsigned)a);
          return;
      } }
#endif

    const uint8_t* orig_addr = rdp.loaded_texture[tile].addr;
    uint32_t orig_size = rdp.loaded_texture[tile].size_bytes;
    uint32_t staged = gfx_gather_texture(tile);
    if (staged == 0) return;
    rdp.loaded_texture[tile].addr = sTexStage; /* decoders read the logical-order staging copy */
    rdp.loaded_texture[tile].size_bytes = staged;
    int t0 = get_time();
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            goto unsupported;
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            goto unsupported;
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            goto unsupported;
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            goto unsupported;
        }
    } else {
    unsupported:
        fprintf(stderr, "[gfx] unsupported texture fmt %d siz %d (skipped)\n", fmt, siz);
    }
    int t1 = get_time();
    //printf("Time diff: %d\n", t1 - t0);
    rdp.loaded_texture[tile].addr = orig_addr;
    rdp.loaded_texture[tile].size_bytes = orig_size;
#ifdef PORT_TEXDUMP
    texdump_record(rdp.loaded_texture[tile].addr, fmt, siz, rdp.loaded_texture[tile].size_bytes,
                   sImpTile->line_size_bytes);
#endif
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4];
#ifndef GBI_FLOATS
    /* PORT: OoT (Matrix_MtxFToMtx) stores the Mtx as 16 int16 integer parts
       (halfwords 0..15) followed by 16 uint16 fractional parts (halfwords
       16..31). Read each 16-bit half individually so host endianness is
       irrelevant (reading them as int32 pairs flips the halves on LE). */
    {
        const int16_t *ip = (const int16_t *)addr;
        const uint16_t *fp = (const uint16_t *)addr + 16;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                int k = i * 4 + j;
                /* guMtxF2L packs 2 elements per 32-bit word; on a little-endian
                   host the two 16-bit halves land swapped, so element k lives at
                   halfword (k ^ 1). */
                int sk = k ^ 1;
                int32_t fixed = ((int32_t)ip[sk] << 16) | fp[sk];
                matrix[i][j] = fixed / 65536.0f;
            }
        }
    }
#else
    memcpy(matrix, addr, sizeof(matrix));
#endif
    
    { /* PORT rawmtx */
        extern unsigned int gfx_port_frame_index;
        static int rt = -2;
        if (rt == -2) { const char* e = getenv("PORT_RAWMTX"); rt = e ? atoi(e) : -1; }
        if (rt >= 0 && (int)gfx_port_frame_index == rt && (parameters & 4)) {
            const uint16_t* a = (const uint16_t*)addr;
            fprintf(stderr, "[RAW params=%02x] int:", parameters);
            for (int z = 0; z < 16; z++) fprintf(stderr, " %04x", a[z]);
            fprintf(stderr, " | frac:");
            for (int z = 16; z < 32; z++) fprintf(stderr, " %04x", a[z]);
            fprintf(stderr, "\n");
        }
    }
    { /* PORT loaded-mtx dump */
        extern unsigned int gfx_port_frame_index;
        static int tgt = -2;
        if (tgt == -2) { const char* e = getenv("PORT_MTXLOG"); tgt = e ? atoi(e) : -1; }
        if (tgt >= 0 && (int)gfx_port_frame_index == tgt) {
            fprintf(stderr, "[LOADMTX %s params=%02x]\n",
                    (parameters & G_MTX_PROJECTION) ? "PROJ" : "MV", parameters);
            for (int r = 0; r < 4; r++)
                fprintf(stderr, "   %9.4f %9.4f %9.4f %9.4f\n",
                        matrix[r][0], matrix[r][1], matrix[r][2], matrix[r][3]);
        }
    }
    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * (4.0f / 3.0f) / ((float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height);
}

static void gfx_sp_vertex_impl(size_t n_vertices, size_t dest_index, const Vtx *vertices);
static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    uint64_t t0 = PERF_T();
    gfx_sp_vertex_impl(n_vertices, dest_index, vertices);
    gPortPerfVtx += PERF_T() - t0;
}

static void gfx_sp_vertex_impl(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        
        float x = v->ob[0] * rsp.MP_matrix[0][0] + v->ob[1] * rsp.MP_matrix[1][0] + v->ob[2] * rsp.MP_matrix[2][0] + rsp.MP_matrix[3][0];
        float y = v->ob[0] * rsp.MP_matrix[0][1] + v->ob[1] * rsp.MP_matrix[1][1] + v->ob[2] * rsp.MP_matrix[2][1] + rsp.MP_matrix[3][1];
        float z = v->ob[0] * rsp.MP_matrix[0][2] + v->ob[1] * rsp.MP_matrix[1][2] + v->ob[2] * rsp.MP_matrix[2][2] + rsp.MP_matrix[3][2];
        float w = v->ob[0] * rsp.MP_matrix[0][3] + v->ob[1] * rsp.MP_matrix[1][3] + v->ob[2] * rsp.MP_matrix[2][3] + rsp.MP_matrix[3][3];
        
        /* PORT (2026-09-27): the RDP rasterizes triangles half a pixel down-right of texture rectangles at
         * the same coordinates; drawing both with one convention put all 3D geometry half an N64 pixel
         * up-left of the N64's (measured with tools/statediff/fbshift.py: minimum at exactly one 2x2
         * supersample in both axes in 3D scenes, none for 2D backgrounds). */
        x += rsp.half_px_x * w;
        y -= rsp.half_px_y * w;
        x = gfx_adjust_x_for_aspect_ratio(x);
        
        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;
        
        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                rsp.lights_changed = false;
            }
            
            { extern unsigned int gfx_port_frame_index; static int lt=-2;
              if(lt==-2){const char*e=getenv("PORT_LIGHTLOG");lt=e?atoi(e):-1;}
              static int shown=-1; if(lt>=0 && (int)gfx_port_frame_index==lt && shown!=lt){ shown=lt;
                fprintf(stderr,"[LIGHT] numlights=%d ambient=(%d,%d,%d)\n",
                  rsp.current_num_lights,
                  rsp.current_lights[rsp.current_num_lights-1].col[0],
                  rsp.current_lights[rsp.current_num_lights-1].col[1],
                  rsp.current_lights[rsp.current_num_lights-1].col[2]); } }
            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
            
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }
            
            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;
            
            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                
                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        
        d->u = U;
        d->v = V;
        
        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= 1;
        if (x > w) d->clip_rej |= 2;
        if (y < -w) d->clip_rej |= 4;
        if (y > w) d->clip_rej |= 8;
        /* no near-plane rejection: OoT's F3DZEX2 is the NoN (no near clipping) microcode */
        if (z > w) d->clip_rej |= 32;
        
        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;
        
        if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }
            
            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = 32767.0f;
            }
            
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            d->color.a = fog_z; // Use alpha variable to store fog factor
        } else {
            d->color.a = v->cn[3];
        }
    }
}

/* PORT (2026-09-25): N64 triangle setup. The RSP clips against the near plane in clip space
 * (attributes linear in clip space) and the RDP then interpolates shade - and fog, which rides in shade
 * alpha - LINEARLY IN SCREEN SPACE, while texture coordinates and depth are perspective-correct. The
 * PICA interpolates everything perspective-correctly, which on large near-camera triangles (floors,
 * terrain) visibly moves shading and fog (tools/statediff fbdiff: Fire Temple floor ~25% too dark,
 * top error in Kokiri Forest). Triangles whose w varies a lot are clipped like the RSP does and split
 * at screen-space edge midpoints: new vertices take screen-linear shade and perspective-correct
 * position/uv, so inside each piece both interpolations agree. Small or flat triangles pass through. */
typedef struct {
    float x, y, z, w; /* clip space (N64 z) */
    float uv[2][2];   /* per texture unit, normalized to the uploaded texture */
    float c[4];       /* shade rgba 0..1 */
} PVtx;

#define SUBDIV_MAX_RATIO 1.15f /* w ratio along an edge below which interpolation differences are < ~3.5% */
#define SUBDIV_MIN_PIXELS 10.0f
#define SUBDIV_MAX_DEPTH 6

static void gfx_pack_tri(const PVtx* t[3], bool z_is_from_0_to_1) {
    if (buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
    for (int i = 0; i < 3; i++) {
        const PVtx* p = t[i];
        float z = p->z, w = p->w;
        if (z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }
        buf_vbo[buf_vbo_len++] = p->x;
        buf_vbo[buf_vbo_len++] = p->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;
        buf_vbo[buf_vbo_len++] = p->uv[0][0];
        buf_vbo[buf_vbo_len++] = p->uv[0][1];
        buf_vbo[buf_vbo_len++] = p->uv[1][0];
        buf_vbo[buf_vbo_len++] = p->uv[1][1];
        buf_vbo[buf_vbo_len++] = p->c[0];
        buf_vbo[buf_vbo_len++] = p->c[1];
        buf_vbo[buf_vbo_len++] = p->c[2];
        buf_vbo[buf_vbo_len++] = p->c[3];
    }
    if (++buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

/* screen-space midpoint of a-b: perspective-correct position/uv, screen-linear shade */
static void pvtx_mid_screen(const PVtx* a, const PVtx* b, PVtx* m) {
    float qa = 1.0f / a->w, qb = 1.0f / b->w, q = 0.5f * (qa + qb), w = 1.0f / q;
    m->x = 0.5f * (a->x * qa + b->x * qb) * w;
    m->y = 0.5f * (a->y * qa + b->y * qb) * w;
    m->z = 0.5f * (a->z * qa + b->z * qb) * w;
    m->w = w;
    for (int i = 0; i < 4; i++) {
        m->uv[i >> 1][i & 1] = 0.5f * (a->uv[i >> 1][i & 1] * qa + b->uv[i >> 1][i & 1] * qb) * w;
    }
    for (int i = 0; i < 4; i++) {
        m->c[i] = 0.5f * (a->c[i] + b->c[i]);
    }
}

/* how much an edge needs splitting: screen length in N64 pixels, if its w ratio is significant */
static float pvtx_edge_score(const PVtx* a, const PVtx* b) {
    float hi = a->w > b->w ? a->w : b->w, lo = a->w > b->w ? b->w : a->w;
    if (hi < SUBDIV_MAX_RATIO * lo) { /* common case: no division */
        return 0.0f;
    }
    float r = hi / lo;
    float dx = (a->x / a->w - b->x / b->w) * (SCREEN_WIDTH / 2), dy = (a->y / a->w - b->y / b->w) * (SCREEN_HEIGHT / 2);
    float len = sqrtf(dx * dx + dy * dy);
    return len < SUBDIV_MIN_PIXELS ? 0.0f : len * (r - 1.0f);
}

static void gfx_subdiv_tri(const PVtx* a, const PVtx* b, const PVtx* c, int depth, bool zf) {
    const PVtx* t[3] = { a, b, c };
    int best = -1;
    float bestScore = 0.0f;
    if (depth < SUBDIV_MAX_DEPTH) {
        for (int e = 0; e < 3; e++) {
            float sc = pvtx_edge_score(t[e], t[(e + 1) % 3]);
            if (sc > bestScore) {
                bestScore = sc;
                best = e;
            }
        }
    }
    if (best < 0) {
        gfx_pack_tri(t, zf);
        return;
    }
    const PVtx *e0 = t[best], *e1 = t[(best + 1) % 3], *opp = t[(best + 2) % 3];
    PVtx m;
    pvtx_mid_screen(e0, e1, &m);
    gfx_subdiv_tri(e0, &m, opp, depth + 1, zf); /* same winding as (e0, e1, opp) */
    gfx_subdiv_tri(&m, e1, opp, depth + 1, zf);
}

static void pvtx_lerp_clip(const PVtx* a, const PVtx* b, float t, PVtx* o) {
    o->x = a->x + (b->x - a->x) * t;
    o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t;
    o->w = a->w + (b->w - a->w) * t;
    for (int i = 0; i < 4; i++) {
        o->uv[i >> 1][i & 1] = a->uv[i >> 1][i & 1] + (b->uv[i >> 1][i & 1] - a->uv[i >> 1][i & 1]) * t;
    }
    for (int i = 0; i < 4; i++) {
        o->c[i] = a->c[i] + (b->c[i] - a->c[i]) * t;
    }
}

/* NoN microcode: nothing is clipped or rejected at the near plane; depth in front of it clamps */
static void pvtx_clamp_near(PVtx* p) {
    if (p->z < -p->w) {
        p->z = -p->w;
    }
}

static void gfx_emit_tri(const PVtx* a, const PVtx* b, const PVtx* c, bool zf) {
    const PVtx* t[3] = { a, b, c };
    PVtx poly[9], tmp[9];
    int n = 0;
    /* The RSP clips a triangle when a vertex lies outside the guard band (x, y = +-ratio * w; rooms
     * use ratio 1 = the screen edges): the new edge vertices get attributes interpolated in clip
     * space, and only between those does the RDP interpolate linearly. */
    float r = rsp.clip_ratio ? (float)rsp.clip_ratio : 2.0f;
    bool outside = false;
    for (int i = 0; i < 3; i++) {
        const PVtx* p = t[i];
        outside |= p->w <= 0.0f || p->x < -r * p->w || p->x > r * p->w || p->y < -r * p->w || p->y > r * p->w;
    }
    if (!outside) {
        bool big = false, near = false;
        for (int e = 0; e < 3; e++) {
            big |= pvtx_edge_score(t[e], t[(e + 1) % 3]) > 0.0f;
            near |= t[e]->z < -t[e]->w;
        }
        if (!big && !near) {
            gfx_pack_tri(t, zf);
            return;
        }
        for (int i = 0; i < 3; i++) {
            poly[i] = *t[i];
            pvtx_clamp_near(&poly[i]);
        }
#ifdef __3DS__
        sBatchSub++;
#endif
        gfx_subdiv_tri(&poly[0], &poly[1], &poly[2], 0, zf);
        return;
    }
#ifdef __3DS__
    sBatchBehind++; /* counts guard-band clips */
#endif
    for (int i = 0; i < 3; i++) {
        poly[n++] = *t[i];
    }
    for (int plane = 0; plane < 5 && n >= 3; plane++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const PVtx *p = &poly[i], *q = &poly[(i + 1) % n];
            float dp, dq;
            switch (plane) {
                case 0: dp = r * p->w + p->x; dq = r * q->w + q->x; break;
                case 1: dp = r * p->w - p->x; dq = r * q->w - q->x; break;
                case 2: dp = r * p->w + p->y; dq = r * q->w + q->y; break;
                case 3: dp = r * p->w - p->y; dq = r * q->w - q->y; break;
                default: dp = p->w - 1e-3f; dq = q->w - 1e-3f; break; /* the eye itself */
            }
            if (dp >= 0.0f) {
                tmp[m++] = *p;
            }
            if ((dp >= 0.0f) != (dq >= 0.0f) && m < 9) {
                pvtx_lerp_clip(p, q, dp / (dp - dq), &tmp[m++]);
            }
        }
        memcpy(poly, tmp, sizeof(PVtx) * m);
        n = m;
    }
    for (int i = 0; i < n; i++) {
        pvtx_clamp_near(&poly[i]);
    }
    for (int i = 1; i + 1 < n; i++) {
        gfx_subdiv_tri(&poly[0], &poly[i], &poly[i + 1], 0, zf);
    }
}

/* PORT (2026-09-28): route draws to the color image's target: the screen, or an off-screen render target
 * read back into the game's buffer after the frame (gfx_3ds.c Port3ds_SetDrawTarget). Off-screen targets
 * are a 1:1 320x240 N64-pixel space: no pillarbox, no aspect squeeze, no supersampling. */
static void* sDrawTarget; /* NULL = screen */
static void gfx_apply_viewport(void);
static void gfx_apply_scissor(void);
static void gfx_select_target(void) {
#ifdef __3DS__
    extern void Port3ds_SetDrawTarget(void* addr, int width, int height);
    void* want = gfx_cimg_is_screen() ? NULL : rdp.color_image_address;
    if (want == sDrawTarget) {
        return;
    }
    gfx_flush();
    sDrawTarget = want;
    if (want != NULL) {
        gfx_current_dimensions.width = SCREEN_WIDTH;
        gfx_current_dimensions.height = SCREEN_HEIGHT;
    } else {
        gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    }
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
    Port3ds_SetDrawTarget(want, (int)rdp.color_image_width, (int)((rdp.scissor_raw[3] + 3) / 4));
    gfx_apply_viewport();
    gfx_apply_scissor();
    memset(&rendering_state.viewport, 0xFF, sizeof(rendering_state.viewport)); /* force re-send */
    memset(&rendering_state.scissor, 0xFF, sizeof(rendering_state.scissor));
#endif
}

/* PORT PERF (2026-09-30): per-triangle render-state setup (target, depth, viewport/scissor, combiner and
 * shader, constants, blending, textures and samplers) only changes when a display-list command changes
 * state. gfx_run_dl clears sTriStateOk on every command except vertex/triangle/matrix/DL-flow ones; while
 * it is set, triangles reuse the values derived for the previous one (sTC) and skip straight to
 * building their vertices. */
static bool sTriStateOk;
static struct {
    bool used_textures[2];
    const struct TileDesc* utile[2];
    float inv_tex_w[2], inv_tex_h[2];
    float u_scale[2], v_scale[2], u_off[2], v_off[2];
    bool linear_filter;
    bool z_is_from_0_to_1;
} sTC;

static void gfx_sp_tri1_impl(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx);
static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    uint64_t t0 = PERF_T(), f0 = gPortPerfFlush;
    gfx_sp_tri1_impl(vtx1_idx, vtx2_idx, vtx3_idx);
    gPortPerfTri += (PERF_T() - t0) - (gPortPerfFlush - f0);
}

static void gfx_sp_tri1_impl(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    gfx_select_target();
    gfx_port_tri_count++;
#ifdef __3DS__
    { extern u32 gPortPerfTrisIn; gPortPerfTrisIn++; }
#endif
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};
    {
        static int trilog_target = -2;
        if (trilog_target == -2) {
            const char* e = getenv("PORT_TRILOG");
            trilog_target = e ? atoi(e) : -1;
        }
        if (trilog_target >= 0 && (int)gfx_port_frame_index == trilog_target
            && gfx_port_tri_count <= 8) {
            if (gfx_port_tri_count == 1) { /* PORT MP dump */
                float (*m)[4] = rsp.MP_matrix;
                for (int r = 0; r < 4; r++)
                    fprintf(stderr, "[MP] %8.3f %8.3f %8.3f %8.3f\n",
                            m[r][0], m[r][1], m[r][2], m[r][3]);
            }
            fprintf(stderr,
                "[tri %u] ndc=(%.2f,%.2f,%.2f)(%.2f,%.2f,%.2f)(%.2f,%.2f,%.2f) "
                "clip=%x|%x|%x cc=%08x gm=%08x\n",
                gfx_port_tri_count,
                v1->x/v1->w, v1->y/v1->w, v1->z/v1->w,
                v2->x/v2->w, v2->y/v2->w, v2->z/v2->w,
                v3->x/v3->w, v3->y/v3->w, v3->z/v3->w,
                v1->clip_rej, v2->clip_rej, v3->clip_rej,
                (uint32_t)rdp.combine_mode, rsp.geometry_mode);
        }
    }
    
    //if (rand()%2) return;
    
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        // The whole triangle lies outside the visible area
        return;
    }
    
    /* PORT PERF: cache the debug env lookup — this is per-triangle; an uncached getenv()
     * here scans the environment for every tri (thousands/frame) and crushes emulated fps. */
    static int nocull = -2;
    if (nocull == -2) nocull = getenv("PORT_NOCULL") ? 1 : 0;
    if (nocull) { /* skip */ } else
    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
        /* PORT PERF: one reciprocal per vertex (VFP divides are slow on the ARM11) */
        float q1 = 1.0f / v1->w, q2 = 1.0f / v2->w, q3 = 1.0f / v3->w;
        float dx1 = v1->x * q1 - v2->x * q2;
        float dy1 = v1->y * q1 - v2->y * q2;
        float dx2 = v3->x * q3 - v2->x * q2;
        float dy2 = v3->y * q3 - v2->y * q2;
        float cross = dx1 * dy2 - dy1 * dx2;
        
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }
        
        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) return;
                break;
            case G_CULL_BACK:
                if (cross >= 0) return;
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return;
        }
    }
    
    if (sTriStateOk) {
        goto emit_vertices; /* render state unchanged since the previous triangle */
    }

    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
    bool two_cycle = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;

    /* PORT (2026-09-24): alpha semantics follow libultraship's Interpreter::GfxSpTri1 (MIT):
     *  - blend: a blender cycle mixes with memory via (1 - A)  [was: "A_MEM not selected"];
     *  - use_alpha (alpha combiner active) = blend || texture_edge;
     *  - cutout: texture_edge, or G_AC_THRESHOLD on a blended draw. The backend picks the alpha
     *    test from these + the blend state (edge: keep a>0.19, drawn opaque; threshold: a>=8/256).
     * Opaque draws keep blending AND alpha test off, so their alpha is irrelevant (the key makes it 1). */
    bool blend = (((rdp.other_mode_l & (3U << 20)) == (G_BL_CLR_MEM << 20)) && ((rdp.other_mode_l & (3U << 16)) == (G_BL_1MA << 16))) ||
                 (((rdp.other_mode_l & (3U << 22)) == (G_BL_CLR_MEM << 22)) && ((rdp.other_mode_l & (3U << 18)) == (G_BL_1MA << 18)));
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool alpha_threshold = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    bool use_alpha = blend || texture_edge;
    bool cutout = texture_edge || (alpha_threshold && blend);

    /* the key only changes with the combine words and the two flags: decode once per change */
    static uint64_t sKeyRaw = ~0ull, sKeyVal;
    static uint32_t sKeyFlags = ~0u, sKeyUsage;
    uint32_t keyFlags = (two_cycle ? 1 : 0) | (use_alpha ? 2 : 0);
    if (rdp.combine_mode != sKeyRaw || keyFlags != sKeyFlags) {
        sKeyRaw = rdp.combine_mode;
        sKeyFlags = keyFlags;
        sKeyVal = gfx_cc_key(two_cycle, use_alpha);
        sKeyUsage = gfx_cc_usage(sKeyVal);
    }
    uint64_t cc_id0 = sKeyVal;
    uint32_t usage = sKeyUsage;
    uint32_t cc_id1 = (usage & (SHADER_OPT_TEX0 | SHADER_OPT_TEX1)) | (use_alpha ? SHADER_OPT_ALPHA : 0) |
                      (use_fog ? SHADER_OPT_FOG : 0) | (cutout ? SHADER_OPT_TEXTURE_EDGE : 0);
    if (rendering_state.shader_program == NULL || cc_id0 != rendering_state.cc_id0 || cc_id1 != rendering_state.cc_id1) {
        gfx_flush();
        struct ShaderProgram *prg = gfx_rapi->lookup_shader(cc_id0, cc_id1);
        gfx_rapi->unload_shader(rendering_state.shader_program);
        if (prg == NULL) {
            prg = gfx_rapi->create_and_load_new_shader(cc_id0, cc_id1);
        } else {
            gfx_rapi->load_shader(prg);
        }
        rendering_state.shader_program = prg;
        rendering_state.cc_id0 = cc_id0;
        rendering_state.cc_id1 = cc_id1;
    }
    struct ShaderProgram *prg = rendering_state.shader_program;

    /* constant inputs (only the ones the key reads, so unrelated color changes don't split batches) */
    struct GfxCombineConsts consts;
    memset(&consts, 0, sizeof(consts));
    if (usage & ((1u << (8 + CCS_PRIM)) | (1u << (8 + CCS_PRIMA)))) {
        memcpy(consts.prim, &rdp.prim_color, 4);
    }
    if (usage & ((1u << (8 + CCS_ENV)) | (1u << (8 + CCS_ENVA)))) {
        memcpy(consts.env, &rdp.env_color, 4);
    }
    if (use_fog) {
        memcpy(consts.fog, &rdp.fog_color, 4);
    }
    if (usage & (1u << (8 + CCS_PRIMLODF))) {
        consts.prim_lod_frac = rdp.prim_lod_frac;
    }
    if (usage & (1u << (8 + CCS_LODF))) { /* fast3d approximation: by distance */
        float distance_frac = (v1->w - 3000.0f) / 3000.0f;
        if (distance_frac < 0.0f) distance_frac = 0.0f;
        if (distance_frac > 1.0f) distance_frac = 1.0f;
        consts.lod_frac = distance_frac * 255.0f;
    }
    if (memcmp(&consts, &rendering_state.consts, sizeof(consts)) != 0) {
        gfx_flush();
        gfx_rapi->set_combine_consts(&consts);
        rendering_state.consts = consts;
    }

    if (blend != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(blend); /* blending on/off; alpha combiner is a shader option */
        rendering_state.alpha_blend = blend;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);

    /* texture unit i samples tile first_tile + i (no LOD: unit 1 stays on the base tile when the
     * first tile is >= 2, as in libultraship) */
    const struct TileDesc* utile[2];
    float tex_width[2] = { 1.0f, 1.0f }, tex_height[2] = { 1.0f, 1.0f };
    bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    for (int i = 0; i < 2; i++) {
        int ti = (i == 1 && rdp.first_tile >= 2) ? rdp.first_tile : (rdp.first_tile + i) & 7;
        utile[i] = &rdp.tiles[ti];
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i, ti);
                rdp.textures_changed[i] = false;
            }
            const struct TileDesc* t = utile[i];
            struct TextureHashmapNode* node = rendering_state.textures[i];
            if (linear_filter != node->linear_filter || t->cms != node->cms || t->cmt != node->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, t->cms, t->cmt);
                node->linear_filter = linear_filter;
                node->cms = t->cms;
                node->cmt = t->cmt;
            }
            if (node->width != 0 && node->height != 0) {
                tex_width[i] = node->width;
                tex_height[i] = node->height;
            } else {
                tex_width[i] = (t->lrs - t->uls + 4) / 4;
                tex_height[i] = (t->lrt - t->ult + 4) / 4;
            }
        }
    }
    
    bool z_is_from_0_to_1 = gfx_rapi->z_is_from_0_to_1();
#ifdef __3DS__
    port_draw_snapshot(); /* first triangle of a batch: record the state it's drawn with */
#endif
    
    sTC.inv_tex_w[0] = 1.0f / tex_width[0];
    sTC.inv_tex_w[1] = 1.0f / tex_width[1];
    sTC.inv_tex_h[0] = 1.0f / tex_height[0];
    sTC.inv_tex_h[1] = 1.0f / tex_height[1];
    sTC.used_textures[0] = used_textures[0];
    sTC.used_textures[1] = used_textures[1];
    sTC.utile[0] = utile[0];
    sTC.utile[1] = utile[1];
    for (int t = 0; t < 2; t++) { /* libultraship's per-tile coordinate transform, as one multiply-add */
        const struct TileDesc* tl = utile[t];
        float su = 1.0f / 32.0f, sv = 1.0f / 32.0f;
        if (tl->shifts != 0) {
            su = tl->shifts <= 10 ? su / (1 << tl->shifts) : su * (1 << (16 - tl->shifts));
        }
        if (tl->shiftt != 0) {
            sv = tl->shiftt <= 10 ? sv / (1 << tl->shiftt) : sv * (1 << (16 - tl->shiftt));
        }
        sTC.u_scale[t] = su;
        sTC.v_scale[t] = sv;
        sTC.u_off[t] = tl->uls / 4.0f;
        sTC.v_off[t] = tl->ult / 4.0f;
    }
    sTC.linear_filter = linear_filter;
    sTC.z_is_from_0_to_1 = z_is_from_0_to_1;
    /* LOD fraction is derived from each triangle's w: keep re-evaluating state for those draws */
    sTriStateOk = !(usage & (1u << (8 + CCS_LODF)));

emit_vertices:;
    PVtx pv[3];
    for (int i = 0; i < 3; i++) {
        PVtx* p = &pv[i];
        p->x = v_arr[i]->x;
        p->y = v_arr[i]->y;
        p->z = v_arr[i]->z;
        p->w = v_arr[i]->w;
        for (int t = 0; t < 2; t++) {
            float u = 0.0f, v = 0.0f;
            if (sTC.used_textures[t]) { /* libultraship's per-tile texture coordinates */
                const struct TileDesc* tl = sTC.utile[t];
                /* (u / 32) shifted, minus the tile origin: scale/offset precomputed per state (sTC) */
                u = v_arr[i]->u * sTC.u_scale[t] - sTC.u_off[t];
                v = v_arr[i]->v * sTC.v_scale[t] - sTC.v_off[t];
                (void)tl;
                if (sTC.linear_filter) {
                    // Linear filter adds 0.5f to the coordinates (texture rectangles too: this
                    // backend's rectangle UVs need it; without it HUD digits blurred - fbdiff)
                    u += 0.5f;
                    v += 0.5f;
                }
                u *= sTC.inv_tex_w[t];
                v *= sTC.inv_tex_h[t];
            }
            p->uv[t][0] = u;
            p->uv[t][1] = v;
        }
        /* shade (with G_FOG the RSP's fog factor replaces shade alpha, as on the N64) */
        p->c[0] = v_arr[i]->color.r * (1.0f / 255.0f);
        p->c[1] = v_arr[i]->color.g * (1.0f / 255.0f);
        p->c[2] = v_arr[i]->color.b * (1.0f / 255.0f);
        p->c[3] = v_arr[i]->color.a * (1.0f / 255.0f);
    }
    {
        uint64_t te = PERF_T(), fe = gPortPerfFlush;
        gfx_emit_tri(&pv[0], &pv[1], &pv[2], sTC.z_is_from_0_to_1);
        gPortPerfEmit += (PERF_T() - te) - (gPortPerfFlush - fe);
    }
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);
    rsp.half_px_x = width > 0.0f ? 1.0f / width : 0.0f;
    rsp.half_px_y = height > 0.0f ? 1.0f / height : 0.0f;
    rdp.vp_raw[0] = x;
    rdp.vp_raw[1] = y;
    rdp.vp_raw[2] = width;
    rdp.vp_raw[3] = height;
    gfx_apply_viewport();
}

/* rdp.viewport from the raw N64 viewport, for the current target's dimensions */
static void gfx_apply_viewport(void) {
    float x = rdp.vp_raw[0], y = rdp.vp_raw[1], width = rdp.vp_raw[2], height = rdp.vp_raw[3];
    
    /* PORT (2026-09-25): N64 screen x maps to x * RATIO_Y + pillar (4:3 content centered in the wider
     * target), and gfx_sp_vertex squeezes clip x by the aspect ratio around the viewport's center, so
     * the viewport keeps a widened width but must be CENTERED on the mapped N64 center. Scaling x by
     * RATIO_X (sm64) is only right for viewports centered on screen: the A button's 45-pixel viewport
     * landed ~12 px off (found by tools/statediff fbdiff). */
    float center = (x + width / 2.0f) * RATIO_Y + GFX_PILLAR_X;
    width *= RATIO_X;
    height *= RATIO_Y;
    x = center - width / 2.0f;
    y *= RATIO_Y;
    
    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_SEGMENT:
            gfx_port_segments[offset / 4] = data;
            break;
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
            if (rsp.current_num_lights > MAX_LIGHTS + 1) {
                rsp.current_num_lights = MAX_LIGHTS + 1;
            }
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_CLIP: /* 4 words: FR_NEG_FRUSTRATIO_n = n, FR_POS_FRUSTRATIO_n = 0x10000 - n */
            rsp.clip_ratio = (uint8_t)((data & 0xFFFF) > 0x8000 ? 0x10000 - (data & 0xFFFF) : data);
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    if (rdp.first_tile != (tile & 7)) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile = tile & 7;
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    rdp.scissor_raw[0] = ulx;
    rdp.scissor_raw[1] = uly;
    rdp.scissor_raw[2] = lrx;
    rdp.scissor_raw[3] = lry;
    gfx_apply_scissor();
}

/* PORT (2026-09-29): 0 (default) = the N64's 4:3 image centered with black side bars, like the
 * original. 1 = "hor+" widescreen: full-width scissors are widened to the whole 400-pixel target so
 * 3D also draws into the sides; OoT was not designed for it (pre-rendered backgrounds, fades and
 * cutscene fills stay 4:3, geometry outside the N64 view shows), so it is opt-in. */
int gPortWidescreen = 0;
/* pre-rendered rooms draw their background image with S2DEX BG_COPY/BG_1CYC: frames that have one (or
 * whose previous frame had one, which covers draws issued before the background) stay 4:3 */
static int sBgThisFrame, sBgLastFrame;
#define WIDE_ACTIVE() (gPortWidescreen && !sBgThisFrame && !sBgLastFrame && sDrawTarget == NULL)
static int sRectFullWidth; /* gfx_dp_fill_rectangle -> gfx_draw_rectangle: stretch to the whole target */

static void gfx_apply_scissor(void) {
    uint32_t ulx = rdp.scissor_raw[0], uly = rdp.scissor_raw[1], lrx = rdp.scissor_raw[2], lry = rdp.scissor_raw[3];
    /* PORT (2026-09-25): pixels map like the content (x * RATIO_Y + pillar), except that in widescreen
     * mode a full-width scissor keeps the whole target so the widened 3D view fills the sides */
    float x = ulx / 4.0f * RATIO_Y + GFX_PILLAR_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_Y;
    float height = (lry - uly) / 4.0f * RATIO_Y;
    if (WIDE_ACTIVE() && ulx == 0 && lrx >= SCREEN_WIDTH * 4 - 4) {
        x = 0.0f;
        width = gfx_current_dimensions.width;
    }
    
    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width + 1;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    struct TileDesc* t = &rdp.tiles[tile & 7];
    /* libultraship: wrapping without a mask clamps */
    if (cms == G_TX_WRAP && masks == G_TX_NOMASK) {
        cms = G_TX_CLAMP;
    }
    if (cmt == G_TX_WRAP && maskt == G_TX_NOMASK) {
        cmt = G_TX_CLAMP;
    }
    t->fmt = fmt;
    t->siz = siz;
    t->palette = palette;
    t->cms = cms;
    t->cmt = cmt;
    t->shifts = shifts;
    t->shiftt = shiftt;
    t->line_size_bytes = line * 8;
    t->tmem = tmem;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    struct TileDesc* t = &rdp.tiles[tile & 7];
    t->uls = uls;
    t->ult = ult;
    t->lrs = lrs;
    t->lrt = lrt;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t high_index) {
    const uint8_t* src = rdp.texture_to_load.addr;
    uint32_t dest = (rdp.tiles[tile & 7].tmem >= 256) ? rdp.tiles[tile & 7].tmem - 256 : 0; /* TLUT entry index */
    uint32_t count = high_index + 1; /* gDPLoadTLUTCmd carries count - 1 (15 or 255) */
    uint32_t i;
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = src;
    if (src == NULL) {
        return;
    }
    uintptr_t sw = gfx_src_swizzle(src);
    for (i = 0; i < count && dest + i < 256; i++) {
        rdp.tlut[dest + i] = (TEX_SRC_BYTE_X(src + i * 2, sw) << 8) | TEX_SRC_BYTE_X(src + i * 2 + 1, sw);
    }
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

/* CI textures depend on the palette colors they index: part of the texture cache key */
static uint32_t gfx_ci_palette_hash(uint32_t fmt, uint32_t siz) {
    uint32_t h = 2166136261u, i, first, n;
    if (fmt != G_IM_FMT_CI) {
        return 0;
    }
    first = (siz == G_IM_SIZ_4b) ? (uint32_t)(sImpTile->palette & 15) * 16 : 0;
    n = (siz == G_IM_SIZ_4b) ? 16 : 256;
    for (i = 0; i < n; i++) {
        h = (h ^ rdp.tlut[first + i]) * 16777619u;
    }
    return h | 1; /* never 0, which means "not CI" */
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    uint32_t slot = rdp.tiles[tile & 7].tmem != 0; /* TMEM slot (see struct TileDesc) */
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);
    
    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = (lrs + 1) << word_size_shift;
    rdp.loaded_texture[slot].size_bytes = size_bytes;
    if (size_bytes > 4096) { static int _w2=0; if(!_w2){_w2=1; fprintf(stderr, "[gfx] texture >4096B clamped\n");} size_bytes = 4096; }
    rdp.loaded_texture[slot].addr = rdp.texture_to_load.addr;
    rdp.loaded_texture[slot].line_size_bytes = rdp.loaded_texture[slot].size_bytes;
    rdp.loaded_texture[slot].full_image_line_size_bytes = rdp.loaded_texture[slot].size_bytes;
    
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

/* PORT (2026-09-24): G_LOADTILE -- load a sub-rectangle of the texture image. Logic follows
 * libultraship's Interpreter::GfxDpLoadTile (MIT, Kenix3): the loaded texture starts at the
 * sub-rect's first texel, rows are tile_width texels, and the source stride is the full image
 * width from G_SETTIMG. It was unhandled (tools/gbi_audit.py), so tile-loaded textures kept the
 * previous load's data. */
static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    uint32_t word_size_shift = 0, slot;
    uint32_t offset_x, offset_y, tile_width, tile_height, tile_line, full_line;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_16b: word_size_shift = 1; break;
        case G_IM_SIZ_32b: word_size_shift = 2; break;
        default: word_size_shift = 0; break; /* 4b/8b as in libultraship */
    }
    offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    tile_width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    tile_height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
    tile_line = tile_width << word_size_shift;
    full_line = rdp.texture_to_load.width << word_size_shift;
    slot = rdp.tiles[tile & 7].tmem != 0;
    rdp.loaded_texture[slot].addr = rdp.texture_to_load.addr + full_line * offset_y + (offset_x << word_size_shift);
    rdp.loaded_texture[slot].size_bytes = tile_line * tile_height;
    rdp.loaded_texture[slot].line_size_bytes = tile_line;
    rdp.loaded_texture[slot].full_image_line_size_bytes = full_line;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

/* raw combine "(0 - 0) * 0 + D" for both cycles and channels */
#define CC_RAW_D(d_rgb, d_alpha)                                                                         \
    (((uint64_t)(GCCc0w0(G_CCMUX_0, G_CCMUX_0, G_ACMUX_0, G_ACMUX_0) | GCCc1w0(G_CCMUX_0, G_CCMUX_0)) << 32) | \
     (GCCc0w1(G_CCMUX_0, d_rgb, G_ACMUX_0, d_alpha) |                                                       \
      GCCc1w1(G_CCMUX_0, G_ACMUX_0, G_ACMUX_0, d_rgb, G_ACMUX_0, d_alpha)))

static void gfx_dp_set_combine_mode(uint64_t raw) {
    rdp.combine_mode = raw;
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    gfx_select_target();
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }
    
    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;
    
    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    
    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);
    if (sRectFullWidth) { /* widescreen: a full-width fill (fade, letterbox, flash) covers the whole screen */
        ulxf = -1.0f;
        lrxf = 1.0f;
        sRectFullWidth = 0;
    }
    
    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    
    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;
    
    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;
    
    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;
    
    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;
    
    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;
    
    sTriStateOk = false; /* the rectangle's temporary viewport/geometry mode must not be cached... */
    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3);
    sTriStateOk = false; /* ...or reused by the triangles that follow it */

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint64_t saved_combine_mode = rdp.combine_mode;
    uint8_t saved_first_tile = rdp.first_tile;
    if (rdp.first_tile != (tile & 7)) { /* the rectangle draws with its own tile (libultraship) */
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        rdp.first_tile = tile & 7;
    }
    rdp.drawing_rect = true;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(CC_RAW_D(G_CCMUX_TEXEL0, G_ACMUX_TEXEL0));
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    /* PORT (2026-09-27): at exactly 1 texel per pixel the RDP's bilinear filter samples texel centers,
     * i.e. returns the texels unfiltered; here 2x2 supersamples land a quarter texel off-center and
     * bilinear smeared 1-pixel HUD/minimap lines (tools/statediff fbdiff). Point-sample instead. */
    uint32_t saved_omh_filter = rdp.other_mode_h;
    bool one_to_one = (dsdx == (1 << 10) || dsdx == -(1 << 10)) && (dtdy == (1 << 10) || dtdy == -(1 << 10));
    if (one_to_one) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }
    
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
    rdp.drawing_rect = false;
    if (one_to_one) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | (saved_omh_filter & (3U << G_MDSFT_TEXTFILT));
    }
    if (rdp.first_tile != saved_first_tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        rdp.first_tile = saved_first_tile;
    }
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }
    
    /* widescreen: untextured fills spanning the N64's full width stretch to the 400-pixel screen */
    sRectFullWidth = WIDE_ACTIVE() && ulx <= 0 && lrx >= SCREEN_WIDTH * 4;
    uint64_t saved_combine_mode = rdp.combine_mode;
    /* only FILL mode writes the fill color; 1/2-cycle fill rects go through the combiner (prim-colored
     * fades, letterboxes, sky rects) - as in libultraship's GfxDpFillRectangle */
    if (mode == G_CYC_FILL) {
        gfx_dp_set_combine_mode(CC_RAW_D(G_CCMUX_SHADE, G_ACMUX_SHADE));
    }
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
    rdp.color_image_width = width + 1; /* the command carries width - 1 */
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}


static inline void *seg_addr(uintptr_t w1) {
    uintptr_t v;
#ifdef __3DS__
    /* 3DS address model: our host RAM (binary .data, gfx pools, arena) lives at
     * physical-equivalent addresses below 0x20000000. The game/decomp produces
     * three flavours of pointer:
     *   - N64 KSEG0/KSEG1 (0x80000000+): strip to physical (== host address).
     *   - true native host pointer (>= 0x10000000, e.g. linear heap): use as-is.
     *   - low value (< 0x10000000): either a real host pointer in .data, OR an
     *     N64 segmented address. If the segment slot is unset it's native;
     *     otherwise translate base+offset (and strip if the base was KSEG0). */
    if (w1 == 0) {
        return NULL;
    }
    /* NOTE (2026-07-31): a "D7 part 2" that resolved SMALL-offset seg refs into heap-loaded
     * segments (to fix the file-select's seg-1 title_static file slots) was tried and
     * REVERTED — it garbled gameplay scene geometry (scene seg-2 small-offset refs are a
     * mix of genuine refs AND native relocated pointers, indistinguishable by offset size).
     * The file-select's black file slots need a narrower fix (only its specific UI segments)
     * that does not touch scene resolution. See roadmap §5.2d. */
    /* Any pointer INTO the loaded binary image [0x00100000, __end__) is a native
     * relocated pointer (a gGfxPools master-DL branch, or a DL/vertex/texture
     * pointer baked into a native-compiled asset), NOT an N64 segment offset. It
     * must pass through even when its high nibble collides with a SET segment —
     * e.g. asset DL pointers relocated to 0x01ffxxxx (nibble 1) vs a loaded
     * segment 1. Segment-translating them sends the interpreter into the unmapped
     * gap above the image. Genuine segment refs to DMA'd room/object data are
     * >= 0x03000000 (> __end__) and still translate below. Mirror of
     * PortSegmentedToVirtual; subsumes the old gGfxPools-only special case. */
    if (w1 >= 0x00100000u && w1 < (uintptr_t)__end__) {
        return (void *) w1;
    }
    if (w1 >= 0x80000000u) {
        return (void *) (w1 & 0x1FFFFFFFu); /* KSEG0/KSEG1 -> physical/host */
    }
    if (w1 >= 0x10000000u) {
        return (void *) w1; /* native host pointer */
    }
    /* D7 fix (2026-07-31): the 3DS app heap (newlib malloc, where DMA'd assets + the
     * game arena live) is at 0x08000000-0x0FFFFFFF, colliding with OoT segment numbers
     * 8-0xF. A native heap pointer in DL data (e.g. a sub-DL the file-select built in a
     * heap buffer, w1=0x08243670) carries its FULL heap address, so its low 24 bits are
     * large (>= 0x100000, since arena allocations start at ~0x0824xxxx); a genuine
     * seg-8..F reference has a SMALL offset (< 1MB) into its UI/keep asset. Treat the
     * large-offset case as a native pointer BEFORE segment translation — otherwise
     * seg_addr did gfx_port_segments[8]+0x243670 = out-of-bounds garbage -> file-select
     * DL runaways. Small-offset (< 0x100000) values still translate as real seg refs. */
    if (w1 >= 0x08000000u && (w1 & 0x00FFFFFFu) >= 0x00100000u) {
        return (void *) w1;
    }
    {
        uintptr_t base = gfx_port_segments[(w1 << 4) >> 28];
        if (base == 0) {
            return (void *) w1; /* unset segment -> native pointer */
        }
        v = base + (w1 & 0x00FFFFFF);
        if (v >= 0x80000000u) {
            v &= 0x1FFFFFFFu; /* segment base was a KSEG0 address */
        }
        return (void *) v;
    }
#else
    if (w1 >= 0x10000000u || w1 == 0) {
        return (void *) w1; /* native pointer (or NULL) */
    }
    v = gfx_port_segments[(w1 << 4) >> 28] + (w1 & 0x00FFFFFF);
    if (v != 0 && v < 0x10000000u) {
        v += 0x80000000u; /* physical emulated-RDRAM base */
    }
    return (void *) v;
#endif
}

/* G_MTX operand. PORT (2026-09-25): 0x01000000 is the billboard matrix (segment 1 =
 * play->billboardMtx, the only segment-1 reference in OoT, always a gsSPMatrix operand), but it
 * also lies inside the loaded binary, so seg_addr passed it through as a native pointer and every
 * billboarded draw (torch glows, moon, flames, effects) multiplied by wallmaster vertex bytes -
 * found by tools/statediff fbdiff (Fire Temple torch glows as a screen-wide band). */
static inline void *seg_addr_mtx(uintptr_t w1) {
#ifdef __3DS__
    if (w1 == 0x01000000u && gfx_port_segments[1] != 0) {
        uintptr_t v = gfx_port_segments[1];
        return (void *) (v >= 0x80000000u ? v & 0x1FFFFFFFu : v);
    }
#endif
    return seg_addr(w1);
}

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

/* PORT (2026-09-24): S2DEX background rectangles (pre-rendered rooms: Market, shops, houses).
 * The room DL switches microcode with gSPLoadUcodeL(gspS2DEX2d_fifo) and draws with
 * gSPBgRectCopy / gSPBgRect1Cyc. Logic follows libultraship's Gfxs2dexBgCopy/Gfxs2dexBg1cyc (MIT):
 * load the image and draw one textured rectangle -- here in 16-row strips so each strip fits the
 * importer's buffers (the whole 320x240 RGBA16 image is 150 KB). */
static bool sUcodeS2dex = false;
static void gfx_s2dex_bg_rect(const uObjBg* bg, bool copy) {
    if (!sBgThisFrame) {
        sBgThisFrame = 1; /* pre-rendered background: keep this frame 4:3 */
        gfx_flush();
        gfx_apply_scissor();
    }
    const uint8_t* img = seg_addr((uintptr_t)bg->b.imagePtr);
    uint32_t w = bg->b.imageW >> 2, h = bg->b.imageH >> 2, row, n;
    uint32_t line = (w * 2 + 7) >> 3;
    int32_t fx = bg->b.frameX, fy = bg->b.frameY;
    const uint32_t kStrip = 16;
    if (img == NULL || w == 0 || h == 0 || bg->b.imageSiz != G_IM_SIZ_16b) return; /* OoT backgrounds are 16-bit */
#ifdef __3DS__
    { /* stereo: the pre-rendered room sits at a fixed middle depth, not at the screen (see G_NOOP tags) */
        extern void gfx_citro3d_set_stereo_mode(int mode);
        gfx_flush();
        gfx_citro3d_set_stereo_mode(2);
    }
#endif
    for (row = 0; row < h; row += kStrip) {
        n = (h - row < kStrip) ? h - row : kStrip;
        gfx_dp_set_texture_image(bg->b.imageFmt, G_IM_SIZ_16b, w - 1, img);
        gfx_dp_set_tile(bg->b.imageFmt, G_IM_SIZ_16b, line, 0, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
        gfx_dp_load_tile(G_TX_LOADTILE, 0, row << 2, (w - 1) << 2, (row + n - 1) << 2);
        gfx_dp_set_tile(bg->b.imageFmt, G_IM_SIZ_16b, line, 0, G_TX_RENDERTILE, bg->b.imagePal, 0, 0, 0, 0, 0, 0);
        gfx_dp_set_tile_size(G_TX_RENDERTILE, 0, row << 2, (w - 1) << 2, (row + n - 1) << 2);
        if (copy) {
            gfx_dp_texture_rectangle(fx, fy + (int32_t)(row << 2), fx + (int32_t)(w << 2) - 4,
                                     fy + (int32_t)((row + n) << 2) - 4, G_TX_RENDERTILE, 0, (int16_t)(row << 5),
                                     4 << 10, 1 << 10, false);
        } else {
            gfx_dp_texture_rectangle(fx, fy + (int32_t)(row << 2), fx + (int32_t)bg->b.frameW,
                                     fy + (int32_t)((row + n) << 2), G_TX_RENDERTILE, 0, (int16_t)(row << 5),
                                     1 << 10, 1 << 10, false);
        }
    }
#ifdef __3DS__
    {
        extern void gfx_citro3d_set_stereo_mode(int mode);
        gfx_flush();
        gfx_citro3d_set_stereo_mode(0);
    }
#endif
}

static uint32_t rdp_half1; /* G_RDPHALF_1 word for G_BRANCH_Z */

static void gfx_run_dl(Gfx* cmd) {
    int dummy = 0;
#ifdef __3DS__
    extern unsigned PortMem_ReadableEnd(unsigned addr);
    unsigned long steps = 0;
    uintptr_t valid_until = 0; /* cmd reads are safe below this mapped-block end */
#endif
    for (;;) {
#ifdef __3DS__
        /* Crash-proof the walk: an un-terminated or garbage DL (bad seg_addr,
         * missing G_ENDDL, or garbage bytes decoded as a G_DL branch) would read
         * off its buffer into unmapped memory and data-abort. Before reading a
         * command, make sure both of its 8 bytes are inside a mapped block; if
         * cmd has crossed out of the last known-good block, re-probe. One
         * svcQueryMemory per 4KB is negligible. */
        if ((uintptr_t)cmd + sizeof(Gfx) > valid_until) {
            unsigned end = PortMem_ReadableEnd((unsigned)(uintptr_t)cmd);
            if (end == 0 || (uintptr_t)cmd + sizeof(Gfx) > (uintptr_t)end) {
                PortLogFastX("[GFX] stop unmapped DL cmd", (unsigned)(uintptr_t)cmd);
                return;
            }
            valid_until = end;
        }
        /* A single linear DL is at most a few thousand commands; a huge count
         * means a runaway that happens to stay in mapped memory. Bail. */
        if (++steps > 2000000UL) {
            PortLogFastX("[GFX] RUNAWAY stop cmd", (unsigned)(uintptr_t)cmd);
            return;
        }
#endif
        uint32_t opcode = cmd->words.w0 >> 24;
#ifdef __3DS__
        sPortCurCmd = cmd; /* draw attribution: which DL command produced a draw */
#endif
        if (opcode == (uint8_t)G_LOAD_UCODE) { /* track which microcode the DL runs under */
            extern uint64_t gspS2DEX2d_fifoTextStart[];
            sUcodeS2dex = (seg_addr(cmd->words.w1) == (void*)gspS2DEX2d_fifoTextStart);
            ++cmd;
            continue;
        }
        if (sUcodeS2dex && (opcode <= 0x0B || opcode == 0xDA || opcode == 0xDC)) {
            /* S2DEX-only opcodes (same numbers as F3DEX2 VTX/TRI/...): draw backgrounds, skip the rest */
            if (opcode == G_BG_COPY || opcode == G_BG_1CYC) {
                gfx_s2dex_bg_rect((const uObjBg*)seg_addr(cmd->words.w1), opcode == G_BG_COPY);
            }
            ++cmd;
            continue;
        }
#ifdef PORT_GBIAUDIT
        { extern unsigned gPortGbiCounts[256]; gPortGbiCounts[opcode & 0xFF]++; }
#endif
        switch (opcode) { /* commands that cannot change triangle render state keep the fast path */
            case G_VTX:
            case G_TRI1:
            case G_TRI2:
            case G_QUAD:
            case G_MTX:
            case (uint8_t)G_POPMTX:
            case G_DL:
            case (uint8_t)G_ENDDL:
            case (uint8_t)G_NOOP:
#ifdef F3DEX_GBI_2
            case (uint8_t)G_RDPHALF_1:
            case (uint8_t)G_BRANCH_Z:
            case (uint8_t)G_CULLDL:
#endif
                break;
            default:
                sTriStateOk = false;
                break;
        }

        switch (opcode) {
#ifdef __3DS__
            /* PORT (2026-09-30): stereo depth tags (gDPNoOpTag 0x3D5E3D0m; z_vr_box_draw.c): m = 0 normal,
             * 1 infinity (sky), 2 fixed middle depth (pre-rendered rooms). */
            case (uint8_t)G_NOOP:
                if ((cmd->words.w1 & 0xFFFFFF00u) == 0x3D5E3D00u) {
                    extern void gfx_citro3d_set_stereo_mode(int mode);
                    gfx_flush();
                    gfx_citro3d_set_stereo_mode((int)(cmd->words.w1 & 0xFF));
                }
                break;
#endif
            // RSP commands:
            case G_MTX:
#ifdef F3DEX_GBI_2
            {
                uint64_t tm = PERF_T();
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr_mtx(cmd->words.w1));
                gPortPerfMtx += PERF_T() - tm;
            }
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr_mtx(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
#ifdef F3DEX_GBI_2
            /* PORT (2026-09-27): F3DEX2 LOD commands, unhandled before: close-range geometry behind
             * gsSPBranchLessZraw never drew (Gerudo Valley's waterfall; tools/statediff fbdiff).
             * G_RDPHALF_1 carries the branch target; G_BRANCH_Z branches (no return) when the vertex's
             * depth is <= zval, compared in clip-space z like libultraship (Ship of Harkinian). */
            case (uint8_t)G_RDPHALF_1:
                rdp_half1 = cmd->words.w1;
                break;
            case (uint8_t)G_BRANCH_Z: {
                uint32_t vi = C0(0, 12) / 2;
                if (vi < MAX_VERTICES && rsp.loaded_vertices[vi].z <= (float)(int32_t)cmd->words.w1) {
                    cmd = (Gfx *)seg_addr(rdp_half1);
                    --cmd; // increase after break
                }
                break;
            }
            case (uint8_t)G_CULLDL: { /* end the DL if vertices v0..vn are all outside one plane */
                uint32_t v0 = C0(0, 16) / 2, vn = cmd->words.w1 / 2, k;
                uint8_t rej = 0xFF;
                for (k = v0; k <= vn && k < MAX_VERTICES; k++) {
                    rej &= rsp.loaded_vertices[k].clip_rej;
                }
                if (v0 <= vn && vn < MAX_VERTICES && rej != 0) {
                    return;
                }
                break;
            }
#endif
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
#ifdef F3DEX_GBI_2
            /* PORT (2026-09-24): F3DEX2 G_QUAD (gSP1Quadrangle, 411 sites incl. the pause-menu
             * pages) packs two triangles exactly like G_TRI2 and the microcode draws it the same
             * way. It had no case, so every quad was silently skipped (found by tools/gbi_audit.py). */
            case (uint8_t)G_QUAD:
#endif
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            case (uint8_t)G_RDPSETOTHERMODE:
                /* PORT (2026-09-24): gDPSetOtherMode sets the WHOLE other mode in one command
                 * (hi 24 bits in w0, lo word in w1). OoT's Gfx_SetupDL_* presets (z_rcp.c, 72
                 * uses) rely on it; unhandled, every draw after a preset inherited stale state
                 * (e.g. the HUD drew in FILL cycle with the scene's FOG blender -> flat dark boxes). */
                rdp.other_mode_h = cmd->words.w0 & 0x00FFFFFFu;
                rdp.other_mode_l = cmd->words.w1;
                break;
            
            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 12), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                rdp.prim_lod_frac = C0(0, 8);
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(((uint64_t)C0(0, 24) << 32) | cmd->words.w1);
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init();
    gfx_rapi->init();
    
    // Used in the 120 star TAS
}

void gfx_start_frame(void) {
    sTriStateOk = false;
    sBgLastFrame = sBgThisFrame;
    sBgThisFrame = 0;
    gfx_wapi->handle_events();
    sDrawTarget = NULL; /* the backend starts each frame on the screen */
    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
    gfx_apply_scissor(); /* widescreen toggled or a pre-rendered room entered/left */
}

#ifdef PORT_GBIAUDIT
/* DEBUG TOOL (opt-in, PORT_EXTRA=-DPORT_GBIAUDIT): runtime opcode histogram, dumped at frames
 * 600 and 1500 as "GBI op=XX n=count" so tools/gbi_audit.py's static list can be confirmed. */
unsigned gPortGbiCounts[256];
static void gbi_audit_dump(unsigned frame) {
    extern void PortDbgX(const char*, unsigned);
    int i;
    PortDbgX("GBI dump at frame", frame);
    for (i = 0; i < 256; i++) if (gPortGbiCounts[i]) { PortDbgX("GBI op", (unsigned)i); PortDbgX("GBI   n", gPortGbiCounts[i]); }
}
#endif
void gfx_run(Gfx *commands) {
    gfx_port_frame_index++;
#ifdef PORT_GBIAUDIT
    if (gfx_port_frame_index == 600 || gfx_port_frame_index == 1500) gbi_audit_dump(gfx_port_frame_index);
#endif
    {
        int i;
        for (i = 0; i < 16; i++) gfx_port_segments[i] = gSegments[i];
    }
    gfx_sp_reset();
    
    //puts("New frame");
    
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;
    
    double t0 = gfx_wapi->get_time();
    gfx_rapi->start_frame();
    gfx_run_dl(commands);
    gfx_flush();
    { extern void PortGfx_FrameReady(void); PortGfx_FrameReady(); }
    double t1 = gfx_wapi->get_time();
    //printf("Process %f %f\n", t1, t1 - t0);
    gfx_wapi->swap_buffers_begin();
#ifdef __3DS__
    port_draw_log_commit();
#endif
}

void gfx_end_frame(void) {
    if (!dropped_frame) {
        gfx_wapi->swap_buffers_end();
    }
}

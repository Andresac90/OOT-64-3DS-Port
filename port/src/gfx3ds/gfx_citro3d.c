#ifdef TARGET_N3DS

#include "gfx_3ds.h"

#include <stdint.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#define u64 __u64
#define s64 __s64
#define u32 __u32
#define vu32 __vu32
#define vs32 __vs32
#define s32 __s32
#define u16 __u16
#define s16 __s16
#define u8 __u8
#define s8 __s8
#include <3ds/types.h>
#undef u64 
#undef s64 
#undef u32 
#undef vu32 
#undef vs32 
#undef s32 
#undef u16 
#undef s16 
#undef u8
#undef s8

#include <PR/gbi.h>

#include <3ds.h>
#include <citro3d.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"

static DVLB_s* sVShaderDvlb;
static shaderProgram_s sShaderProgram;
static void* sVboBuffer;

extern const u8 shader_shbin[];
extern const u32 shader_shbin_size;

struct ShaderProgram {
    uint32_t shader_id;
    u32 opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    u32 attrib_locations[7];
    uint8_t attrib_sizes[7];
    uint8_t num_attribs;
};

/* PORT: OoT uses many more distinct color-combiner configs than sm64 did. The
 * old [64] pool + uint8_t size overflowed once the game encountered >64 combiners
 * (writing past the array corrupted the adjacent sCurShader global -> OOB index ->
 * crash in updateShader, e.g. after using an item). Enlarge the pool and widen the
 * counter, and hard-guard allocation in gfx_citro3d_create_and_load_new_shader. */
#define SHADER_POOL_CAP 1024
static struct ShaderProgram sShaderProgramPool[SHADER_POOL_CAP];
static uint16_t sShaderProgramPoolSize;

static int sCurShader = 0;

static C3D_Tex sTexturePool[4096];
static float sTexturePoolScaleS[4096];
static float sTexturePoolScaleT[4096];
static int sTextureIndex;
static int sCurTex = 0;

static int sTexUnits[2];

static bool gfx_citro3d_z_is_from_0_to_1(void)
{
    return true;
}

static int sVtxUnitSize = 0;

static bool sDepthTestOn = false;
static bool sDepthUpdateOn = true;
static bool sDepthDecal = false;

static bool sUseBlend;

static int sBufIdx = 0;

static void gfx_citro3d_vertex_array_set_attribs(struct ShaderProgram *prg) {
    int unitSize = 0;
    for (int i = 0; i < prg->num_attribs; i++)
        unitSize += prg->attrib_sizes[i];
    sVtxUnitSize = unitSize;
}

static void gfx_citro3d_unload_shader(struct ShaderProgram *old_prg) {

}

/* Map an N64 combiner colour input to a PICA TEV source. The N64 combiner has up
 * to two per-vertex colour inputs (SHADER_INPUT_1/_2 — resolved by gfx_pc.c from
 * shade/prim/env). The port carries ONE of them per vertex (GPU_PRIMARY_COLOR) and,
 * when a draw needs two (renderTwoColorTris), puts the other in the TEV CONSTANT.
 * `swapInput01` selects which of the pair is the varying (vertex) one. */
static GPU_TEVSRC getTevSrc(int input, bool swapInput01)
{
    switch(input)
    {
        case SHADER_0:
            return GPU_CONSTANT; /* only referenced by funcs that ignore it */
        case SHADER_INPUT_1:
            return swapInput01 ? GPU_CONSTANT : GPU_PRIMARY_COLOR;
        case SHADER_INPUT_2:
            return swapInput01 ? GPU_PRIMARY_COLOR : GPU_CONSTANT;
        case SHADER_INPUT_3:
            return GPU_CONSTANT;
        case SHADER_INPUT_4:
            return GPU_CONSTANT;
        case SHADER_TEXEL0:
        case SHADER_TEXEL0A:
            return GPU_TEXTURE0;
        case SHADER_TEXEL1:
            return GPU_TEXTURE1;
    }
    return GPU_CONSTANT;
}

static void updateShader(bool swapInput01)
{
    /* Defensive: never index the pool out of range (a corrupt sCurShader must
     * degrade to a wrong-but-safe shader, not crash). */
    if (sCurShader < 0 || sCurShader >= SHADER_POOL_CAP) {
        sCurShader = 0;
    }
    struct ShaderProgram* new_prg = &sShaderProgramPool[sCurShader];

    u32 shader_id = new_prg->shader_id;

    uint8_t c[2][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = (shader_id >> (i * 3)) & 7;
        c[1][i] = (shader_id >> (12 + i * 3)) & 7;
    }
    bool opt_alpha = (shader_id & SHADER_OPT_ALPHA) != 0;
    bool opt_fog = (shader_id & SHADER_OPT_FOG) != 0;
    bool opt_texture_edge = (shader_id & SHADER_OPT_TEXTURE_EDGE) != 0;
    bool used_textures[2] = {0, 0};
    int num_inputs = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (c[i][j] >= SHADER_INPUT_1 && c[i][j] <= SHADER_INPUT_4) {
                if (c[i][j] > num_inputs) {
                    num_inputs = c[i][j];
                }
            }
            if (c[i][j] == SHADER_TEXEL0 || c[i][j] == SHADER_TEXEL0A) {
                used_textures[0] = true;
            }
            if (c[i][j] == SHADER_TEXEL1) {
                used_textures[1] = true;
            }
        }
    }
    bool do_single[2] = {c[0][2] == 0, c[1][2] == 0};
    bool do_multiply[2] = {c[0][1] == 0 && c[0][3] == 0, c[1][1] == 0 && c[1][3] == 0};
    bool do_mix[2] = {c[0][1] == c[0][3], c[1][1] == c[1][3]};
    bool color_alpha_same = (shader_id & 0xfff) == ((shader_id >> 12) & 0xfff);

    if(num_inputs >= 3)
        printf("more than 2!\n");

    /* PORT: minimal, correct colour combiner (TEV). OoT scene/actor geometry
     * reduces to "texture modulated by a per-vertex colour" (shade/prim/env — the
     * N64 combiner's colour source, resolved and packed into PRIMARY_COLOR by
     * gfx_pc.c), or a flat colour where there is no texture. The former hand-rolled
     * do_single/multiply/mix + two-colour TEV path mis-programmed the PICA TEV and
     * tinted grayscale surfaces wrong (grass rendered pink); this mapping renders
     * them correctly. Texture-only combiners get PRIMARY_COLOR = white (gfx_pc.c
     * writes 1,1,1,1 when there is no colour input) so TEXEL0 * white == TEXEL0. */
    {
        C3D_TexEnv* e0 = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e0);
        /* Reset the TEV CONSTANT to 0 every shader setup. It is the 2nd colour input
         * for two-colour combiners (overwritten per-draw by renderTwoColorTris) and
         * the value of SHADER_0. Without this reset a shader inherits the CONSTANT the
         * *previous* scene left behind -> the same combiner renders differently in
         * different scenes (e.g. HUD icons correct in town but boxes in Hyrule Field). */
        C3D_TexEnvColor(e0, 0);

        bool has_tex = used_textures[0] || used_textures[1];

        /* --- RGB: map the N64 colour combiner (A-B)*C+D onto one PICA TEV stage. ---
         * A=c[0][0] B=c[0][1] C=c[0][2] D=c[0][3]. Sources resolve via getTevSrc to
         * TEXEL0/1, the varying vertex colour (PRIMARY_COLOR) and a per-draw CONSTANT
         * (the 2nd colour, set by renderTwoColorTris). The func is chosen so that
         * zero terms are never referenced (PICA has no true zero source). TEXEL0A
         * uses the texture's ALPHA as an RGB source. */
        if (has_tex || num_inputs > 0) {
            uint8_t a = c[0][0], b = c[0][1], cc = c[0][2], d = c[0][3];
            GPU_TEVSRC sa = getTevSrc(a, swapInput01), sb = getTevSrc(b, swapInput01),
                       sc = getTevSrc(cc, swapInput01), sd = getTevSrc(d, swapInput01);
            GPU_TEVOP_RGB opa = (a == SHADER_TEXEL0A) ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
            GPU_TEVOP_RGB opb = (b == SHADER_TEXEL0A) ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
            GPU_TEVOP_RGB opc = (cc == SHADER_TEXEL0A) ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
            GPU_TEVOP_RGB opd = (d == SHADER_TEXEL0A) ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
            if (cc == SHADER_0) {
                /* (A-B)*0 + D  ==  D */
                C3D_TexEnvSrc(e0, C3D_RGB, sd, 0, 0);
                C3D_TexEnvOpRgb(e0, opd, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
                C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
            } else if (b == SHADER_0 && d == SHADER_0) {
                /* A * C  (the common "texture modulated by colour") */
                C3D_TexEnvSrc(e0, C3D_RGB, sa, sc, 0);
                C3D_TexEnvOpRgb(e0, opa, opc, GPU_TEVOP_RGB_SRC_COLOR);
                C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
            } else if (b == SHADER_0) {
                /* A * C + D */
                C3D_TexEnvSrc(e0, C3D_RGB, sa, sc, sd);
                C3D_TexEnvOpRgb(e0, opa, opc, opd);
                C3D_TexEnvFunc(e0, C3D_RGB, GPU_MULTIPLY_ADD);
            } else if (d == b) {
                /* (A-B)*C + B  ==  interpolate(A,B,C) = A*C + B*(1-C).
                 * This is the two-colour "mix" (e.g. tunic (PRIM-ENV)*TEXEL0+ENV). */
                C3D_TexEnvSrc(e0, C3D_RGB, sa, sb, sc);
                C3D_TexEnvOpRgb(e0, opa, opb, opc);
                C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
            } else {
                /* fallback: A * C */
                C3D_TexEnvSrc(e0, C3D_RGB, sa, sc, 0);
                C3D_TexEnvOpRgb(e0, opa, opc, GPU_TEVOP_RGB_SRC_COLOR);
                C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
            }
        } else {
            /* No texture and no colour input — pass the (white) vertex colour. */
            C3D_TexEnvSrc(e0, C3D_RGB, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvOpRgb(e0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
        }

        /* --- ALPHA: take alpha from the texture when textured (matches OoT opaque &
         * cutout surfaces and keeps Link visible — modulating by the packed shade
         * alpha wrongly zeroed him); else from the vertex colour. ---
         * NOTE: an earlier attempt to drive this from the decoded alpha mux
         * (build 7E39522E) regressed HW — surfaces whose alpha mux resolved to a
         * zero/CONSTANT source got alpha 0 and were discarded by the alpha test
         * (Link's sword vanished). Reverted to this proven behaviour. A correct
         * alpha combiner needs per-surface validation, not a blanket ladder. */
        /* PORT (2026-09-24): the N64 ALPHA combiner (A-B)*C+D, mapped like the RGB ladder above.
         * Only for draws with the alpha option (translucent / cutout). Opaque draws have blending
         * and alpha test OFF (applyAlphaTest), so their alpha -- whose mux gfx_pc zeroes -- can no
         * longer discard them (the failure mode of the earlier attempt: Link's sword vanished). */
        if (opt_alpha) {
            uint8_t a = c[1][0], b = c[1][1], cc = c[1][2], d = c[1][3];
            GPU_TEVSRC sa = getTevSrc(a, swapInput01), sb = getTevSrc(b, swapInput01),
                       sc = getTevSrc(cc, swapInput01), sd = getTevSrc(d, swapInput01);
            if (cc == SHADER_0 || (a == SHADER_0 && b == SHADER_0)) {
                C3D_TexEnvSrc(e0, C3D_Alpha, sd, 0, 0);                /* D */
                C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
            } else if (b == SHADER_0 && d == SHADER_0) {
                C3D_TexEnvSrc(e0, C3D_Alpha, sa, sc, 0);               /* A*C */
                C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
            } else if (b == SHADER_0) {
                C3D_TexEnvSrc(e0, C3D_Alpha, sa, sc, sd);              /* A*C + D */
                C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MULTIPLY_ADD);
            } else if (b == d) {
                C3D_TexEnvSrc(e0, C3D_Alpha, sa, sb, sc);              /* (A-B)*C + B = lerp */
                C3D_TexEnvFunc(e0, C3D_Alpha, GPU_INTERPOLATE);
            } else {
                C3D_TexEnvSrc(e0, C3D_Alpha, sa, sc, 0);               /* fallback: A*C */
                C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
            }
        } else {
            C3D_TexEnvSrc(e0, C3D_Alpha, has_tex ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
        }
        C3D_TexEnvOpAlpha(e0, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);

        /* --- TEV stage 1: N64 combiner CYCLE 1 fold (see gfx_pc.c). When the DL's
         * cycle 1 is (COMBINED-0)*X+0, gfx_pc routes X through a colour-input slot
         * and encodes: bits 27-29 = X's CC source (nonzero = active), bits 30-31 =
         * input slot - 1. RGB1 = PREVIOUS * srcof(slot); alpha passes through.
         * This restores e.g. the tunic's ENV tint (cycle0 TEX*SHADE was rendering
         * untinted -> white tunic) and terrain's cycle-1 SHADE/PRIM lighting. */
        if ((shader_id >> 27) & 7) {
            int pslot = (int)((shader_id >> 30) & 3) + 1;
            C3D_TexEnv* e1 = C3D_GetTexEnv(1);
            C3D_TexEnvInit(e1);
            C3D_TexEnvColor(e1, 0); /* renderTwoColorTris overwrites per-draw */
            C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, getTevSrc(pslot, swapInput01), 0);
            C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
            C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
            C3D_TexEnvOpAlpha(e1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
            C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
        } else {
            C3D_TexEnvInit(C3D_GetTexEnv(1));
        }
        C3D_TexEnvInit(C3D_GetTexEnv(2));
        /* Alpha test discards fully-transparent texels: opaque surfaces have alpha 1
         * and survive `> 0`; transparent parts of HUD icons/sprites get cut out
         * (without this they draw as solid boxes). Cutout foliage uses a higher
         * threshold. */
        /* alpha test is per draw (depends on blend state): see applyAlphaTest() */
    }
}

static void gfx_citro3d_load_shader(struct ShaderProgram *new_prg) {
    sCurShader = new_prg->opengl_program_id;
    gfx_citro3d_vertex_array_set_attribs(new_prg);

    updateShader(false);
}

static struct ShaderProgram *gfx_citro3d_create_and_load_new_shader(uint32_t shader_id) {
    uint8_t c[2][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = (shader_id >> (i * 3)) & 7;
        c[1][i] = (shader_id >> (12 + i * 3)) & 7;
    }
    bool opt_alpha = (shader_id & SHADER_OPT_ALPHA) != 0;
    bool opt_fog = (shader_id & SHADER_OPT_FOG) != 0;
    bool opt_texture_edge = (shader_id & SHADER_OPT_TEXTURE_EDGE) != 0;
    bool used_textures[2] = {0, 0};
    int num_inputs = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (c[i][j] >= SHADER_INPUT_1 && c[i][j] <= SHADER_INPUT_4) {
                if (c[i][j] > num_inputs) {
                    num_inputs = c[i][j];
                }
            }
            if (c[i][j] == SHADER_TEXEL0 || c[i][j] == SHADER_TEXEL0A) {
                used_textures[0] = true;
            }
            if (c[i][j] == SHADER_TEXEL1) {
                used_textures[1] = true;
            }
        }
    }
    bool do_single[2] = {c[0][2] == 0, c[1][2] == 0};
    bool do_multiply[2] = {c[0][1] == 0 && c[0][3] == 0, c[1][1] == 0 && c[1][3] == 0};
    bool do_mix[2] = {c[0][1] == c[0][3], c[1][1] == c[1][3]};
    bool color_alpha_same = (shader_id & 0xfff) == ((shader_id >> 12) & 0xfff);

    /* two-cycle postmul input (see gfx_pc.c fold): its colour rides an extra
     * vertex-colour slot that the mux bits don't reference — count it so the
     * vertex layout and gfx_pc's packing agree. */
    if ((shader_id >> 27) & 7) {
        int pslot = (int)((shader_id >> 30) & 3) + 1;
        if (pslot > num_inputs) {
            num_inputs = pslot;
        }
    }

    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;
    
    size_t cnt = 0;
    
    if (sShaderProgramPoolSize >= SHADER_POOL_CAP) {
        /* Pool full: reuse the last slot instead of overflowing into adjacent
         * globals. Returns an already-loaded, valid shader — no crash. Only hit
         * if the game uses more than SHADER_POOL_CAP distinct combiner configs. */
        return &sShaderProgramPool[SHADER_POOL_CAP - 1];
    }
    int id = sShaderProgramPoolSize;
    struct ShaderProgram *prg = &sShaderProgramPool[sShaderProgramPoolSize++];
    prg->attrib_sizes[cnt] = 4;
    ++cnt;
    
    if (used_textures[0] || used_textures[1]) 
    {
        prg->attrib_sizes[cnt] = 2;
        ++cnt;
    }
    
    if (opt_fog) 
    {
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }
    
    for (int i = 0; i < num_inputs; i++) 
    {
        prg->attrib_sizes[cnt] = opt_alpha ? 4 : 3;
        ++cnt;
    }
    
    prg->shader_id = shader_id;
    prg->opengl_program_id = id;
    prg->num_inputs = num_inputs;
    prg->used_textures[0] = used_textures[0];
    prg->used_textures[1] = used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    
    gfx_citro3d_load_shader(prg);
    
    return prg;
}

static struct ShaderProgram *gfx_citro3d_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < sShaderProgramPoolSize; i++) {
        if (sShaderProgramPool[i].shader_id == shader_id) {
            return &sShaderProgramPool[i];
        }
    }
    return NULL;
}

static void gfx_citro3d_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static u32 gfx_citro3d_new_texture(void) {
    if(sTextureIndex == 4096)
    {
        printf("Out of texures!");
        return 0;
    }
    return sTextureIndex++;
}

static void gfx_citro3d_select_texture(int tile, u32 texture_id) {
    C3D_TexBind(tile, &sTexturePool[texture_id]);
    sCurTex = texture_id;
    sTexUnits[tile] = texture_id;
}

static u32 sTexBuf[16 * 1024] __attribute__((aligned(32)));

static int sTileOrder[] =
{
    0,  1,   4,  5,
    2,  3,   6,  7,

    8,  9,  12, 13,  
    10, 11,  14, 15
};

static void performTexSwizzle(const u8* src, u32* dst, u32 w, u32 h)
{
    int offs = 0;
    for(int y = 0; y < h; y += 8)
    {
        for(int x = 0; x < w; x += 8)
        {
            for (int i = 0; i < 64; i++)
            {
                int x2 = i & 7;
                int y2 = i >> 3;
                int pos = sTileOrder[(x2 & 3) + ((y2 & 3) << 2)] + ((x2 >> 2) << 4) + ((y2 >> 2) << 5);
                u32 c = ((const u32*)src)[(y + y2) * w + x + x2];
                dst[offs + pos] = ((c & 0xFF) << 24) | (((c >> 8) & 0xFF) << 16) | (((c >> 16) & 0xFF) << 8) | (c >> 24);
            }
            dst += 64;
        }
    }
}

static void gfx_citro3d_upload_texture(uint8_t *rgba32_buf, int width, int height) {
    if(width < 8 || height < 8 || (width & (width - 1)) || (height & (height - 1)))
    {
        int newWidth = width < 8 ? 8 : (1 << (32 - __builtin_clz(width - 1)));
        int newHeight = height < 8 ? 8 : (1 << (32 - __builtin_clz(height - 1)));
        if(newWidth * newHeight * 4 > sizeof(sTexBuf))
        {
            printf("Tex buffer overflow!\n");
            return;
        }
        int offs = 0;
        for(int y = 0; y < newHeight; y += 8)
        {
            for(int x = 0; x < newWidth; x += 8)
            {
                for (int i = 0; i < 64; i++)
                {
                    int x2 = i % 8;
                    int y2 = i / 8;

                    int realX = x + x2;
                    if(realX >= width)
                        realX -= width;

                    int realY = y + y2;
                    if(realY >= height)
                        realY -= height;

                    int pos = sTileOrder[x2 % 4 + y2 % 4 * 4] + 16 * (x2 / 4) + 32 * (y2 / 4);
                    u32 c = ((u32*)rgba32_buf)[realY * width + realX];
                    ((u32*)sTexBuf)[offs + pos] = ((c & 0xFF) << 24) | (((c >> 8) & 0xFF) << 16) | (((c >> 16) & 0xFF) << 8) | (c >> 24);
                }
                offs += 64;
            }
        }
        sTexturePoolScaleS[sCurTex] = width / (float)newWidth;
        sTexturePoolScaleT[sCurTex] = height / (float)newHeight;
        width = newWidth;
        height = newHeight;
    }
    else
    {
        sTexturePoolScaleS[sCurTex] = 1.f;
        sTexturePoolScaleT[sCurTex] = 1.f;
        performTexSwizzle(rgba32_buf, sTexBuf, width, height);
    }
    /* Cache slots get recycled (gfx_pc pool wraps at 512); C3D_TexInit on an
     * already-initialized tex leaks its previous linear buffer. Free it first,
     * or reuse the buffer when the dimensions match. */
    if (sTexturePool[sCurTex].data != NULL &&
        (sTexturePool[sCurTex].width != width || sTexturePool[sCurTex].height != height)) {
        C3D_TexDelete(&sTexturePool[sCurTex]);
        sTexturePool[sCurTex].data = NULL;
    }
    if (sTexturePool[sCurTex].data == NULL) {
        C3D_TexInit(&sTexturePool[sCurTex], width, height, GPU_RGBA8);
    }
    C3D_TexUpload(&sTexturePool[sCurTex], sTexBuf);
    C3D_TexFlush(&sTexturePool[sCurTex]);
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP)
         return GPU_CLAMP_TO_EDGE;
    return (val & G_TX_MIRROR) ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
}

static void gfx_citro3d_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    C3D_TexSetFilter(&sTexturePool[sTexUnits[tile]], linear_filter ? GPU_LINEAR : GPU_NEAREST, linear_filter ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&sTexturePool[sTexUnits[tile]], gfx_cm_to_opengl(cms), gfx_cm_to_opengl(cmt));
}

static void updateDepth()
{
    C3D_DepthTest(sDepthTestOn, GPU_LEQUAL, sDepthUpdateOn ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    C3D_DepthMap(true, sDepthDecal ? -1 : -1, sDepthDecal ? -0.001f : 0);
}

static void gfx_citro3d_set_depth_test(bool depth_test) {
    sDepthTestOn = depth_test;
    updateDepth();
}

static void gfx_citro3d_set_depth_mask(bool z_upd) {
    sDepthUpdateOn = z_upd;
    updateDepth();
}

static void gfx_citro3d_set_zmode_decal(bool zmode_decal) {
    sDepthDecal = zmode_decal;
    updateDepth();
}

static void gfx_citro3d_set_viewport(int x, int y, int width, int height) {
    /* PORT (2026-09-24): the top-screen target is PORTRAIT (240x400, rotated), so x/y and
     * width/height swap -- exactly like gfx_citro3d_set_scissor below and the sm64 3DS port
     * this backend derives from. Passing them unswapped shifted the whole picture ~half a
     * screen to the right ("camera off-center"), which also pushed the pause menu off-screen. */
    if (gGfx3DSMode == GFX_3DS_MODE_AA_22 || gGfx3DSMode == GFX_3DS_MODE_WIDE_AA_12)
        C3D_SetViewport(y * 2, x * 2, height * 2, width * 2);
    else if (gGfx3DSMode == GFX_3DS_MODE_WIDE)
        C3D_SetViewport(y, x * 2, height, width * 2);
    else
        C3D_SetViewport(y, x, height, width);
}

static void gfx_citro3d_set_scissor(int x, int y, int width, int height)
{
    if (gGfx3DSMode == GFX_3DS_MODE_NORMAL)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, x, y + height, x + width);
    else if (gGfx3DSMode == GFX_3DS_MODE_AA_22 || gGfx3DSMode == GFX_3DS_MODE_WIDE_AA_12)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y * 2, x * 2, (y + height) * 2, (x + width) * 2);
    else if (gGfx3DSMode == GFX_3DS_MODE_WIDE)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, x * 2, y + height, (x + width) * 2);
}

/* PORT (2026-09-24): alpha test per draw, libultraship semantics. Opaque: off. Cutout without
 * blending (texture edge): keep alpha > 0.19 and draw opaque. Cutout with blending (threshold):
 * discard alpha < 8/256. Other translucent draws: discard alpha 0 only (keeps depth clean). */
static void applyAlphaTest(void)
{
    u32 id = sShaderProgramPool[sCurShader].shader_id;
    if (!(id & SHADER_OPT_ALPHA))
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    else if (id & SHADER_OPT_TEXTURE_EDGE)
        C3D_AlphaTest(true, GPU_GREATER, sUseBlend ? 7 : 48);
    else
        C3D_AlphaTest(true, GPU_GREATER, 0);
}

static void applyBlend()
{
    if (sUseBlend)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

static void gfx_citro3d_set_use_alpha(bool use_alpha)
{
    sUseBlend = use_alpha;
    applyBlend();
}

static u32 vec4ToU32Color(float r, float g, float b, float a)
{
    int r2 = r * 255;
    if (r2 < 0)
        r2 = 0;
    else if(r2 > 255)
        r2 = 255;
    int g2 = g * 255;
    if (g2 < 0)
        g2 = 0;
    else if(g2 > 255)
        g2 = 255;
    int b2 = b * 255;
    if (b2 < 0)
        b2 = 0;
    else if(b2 > 255)
        b2= 255;
    int a2 = a * 255;
    if (a2 < 0)
        a2 = 0;
    else if(a2 > 255)
        a2 = 255;
    return (a2 << 24) | (b2 << 16) | (g2 << 8) | r2;
}

static void renderFog(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_DST_ALPHA);
    C3D_DepthTest(sDepthTestOn, GPU_LEQUAL, GPU_WRITE_COLOR);

    int offset = 0;
    float* dst = &((float*)sVboBuffer)[sBufIdx * 10];
    bool hasTex = sShaderProgramPool[sCurShader].used_textures[0] || sShaderProgramPool[sCurShader].used_textures[1];
    bool hasFog = (sShaderProgramPool[sCurShader].shader_id & SHADER_OPT_FOG) != 0;
    for(int i = 0; i < 3 * buf_vbo_num_tris; i++)
    {
        *dst++ = buf_vbo[offset + 1];
        *dst++ = -buf_vbo[offset + 0];
        *dst++ = -buf_vbo[offset + 2];
        *dst++ = buf_vbo[offset + 3];
        int vtxOffs = 4;
        if(hasTex)
            vtxOffs += 2;
        *dst++ = 0;
        *dst++ = 0;
        *dst++ = buf_vbo[offset + vtxOffs++];
        *dst++ = buf_vbo[offset + vtxOffs++];
        *dst++ = buf_vbo[offset + vtxOffs++];
        *dst++ = buf_vbo[offset + vtxOffs++];
        
        offset += sVtxUnitSize;
    }

	C3D_DrawArrays(GPU_TRIANGLES, sBufIdx, buf_vbo_num_tris * 3);
    sBufIdx += buf_vbo_num_tris * 3;

    updateShader(false);
    applyBlend();
    updateDepth();
}

static void renderTwoColorTris(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    int offset = 0;
    float* dst = &((float*)sVboBuffer)[sBufIdx * 10];
    bool hasTex = sShaderProgramPool[sCurShader].used_textures[0] || sShaderProgramPool[sCurShader].used_textures[1];
    bool hasColor = sShaderProgramPool[sCurShader].num_inputs > 0;
    bool hasAlpha = (sShaderProgramPool[sCurShader].shader_id & SHADER_OPT_ALPHA) != 0;
    bool hasFog = (sShaderProgramPool[sCurShader].shader_id & SHADER_OPT_FOG) != 0;
    if (hasFog)
        C3D_TexEnvColor(C3D_GetTexEnv(2), vec4ToU32Color(buf_vbo[hasTex ? 6 : 4], buf_vbo[hasTex ? 7 : 5], buf_vbo[hasTex ? 8 : 6], buf_vbo[hasTex ? 9 : 7]));
    u32 firstColor0, firstColor1;
    bool color0Constant = true;
    bool color1Constant = true;
    //determine which color is constant over all vertices
    for(int i = 0; i < buf_vbo_num_tris * 3 && color0Constant && color1Constant; i++)
    {    
        int vtxOffs = 4;
        if(hasTex)
            vtxOffs += 2;
        if(hasFog)
            vtxOffs += 4;
        u32 color0 = vec4ToU32Color(
            buf_vbo[offset + vtxOffs], 
            buf_vbo[offset + vtxOffs + 1],
            buf_vbo[offset + vtxOffs + 2],
            hasAlpha ? buf_vbo[offset + vtxOffs + 3] : 1.0f);
        vtxOffs += hasAlpha ? 4 : 3;
        u32 color1 = vec4ToU32Color(
            buf_vbo[offset + vtxOffs], 
            buf_vbo[offset + vtxOffs + 1],
            buf_vbo[offset + vtxOffs + 2],
            hasAlpha ? buf_vbo[offset + vtxOffs + 3] : 1.0f);
        if(i == 0)
        {
            firstColor0 = color0;
            firstColor1 = color1;
        }
        else
        {
            if(firstColor0 != color0)
                color0Constant = false;
            if(firstColor1 != color1)
                color1Constant = false;
        }
        offset += sVtxUnitSize;
    }
    offset = 0;
    /* The vertex loop below SKIPS colour0 iff color0Constant — so PRIMARY_COLOR
     * carries colour1 in that case and the TEV CONSTANT must be colour0 (and the
     * inputs swap). The old code keyed both choices on color1Constant, so when
     * BOTH colours were constant (every HUD texrect: PRIM+ENV fixed) the CONSTANT
     * was set to colour1 while PRIMARY also carried colour1 — colour0 (e.g. the
     * hearts' PRIM red) was dropped entirely and the interpolation degenerated to
     * the flat ENV tint. Key on which colour is actually packed per-vertex. */
    updateShader(color0Constant);
    {
        u32 constCol = color0Constant ? firstColor0 : firstColor1;
        C3D_TexEnvColor(C3D_GetTexEnv(0), constCol);
        C3D_TexEnvColor(C3D_GetTexEnv(1), constCol); /* cycle-1 postmul stage reads it too */
    }
    for(int i = 0; i < 3 * buf_vbo_num_tris; i++)
    {
        *dst++ = buf_vbo[offset + 1];
        *dst++ = -buf_vbo[offset + 0];
        *dst++ = -buf_vbo[offset + 2];
        *dst++ = buf_vbo[offset + 3];
        int vtxOffs = 4;
        if(hasTex)
        {
            *dst++ = buf_vbo[offset + vtxOffs++] * sTexturePoolScaleS[sCurTex];
            *dst++ = 1 - (buf_vbo[offset + vtxOffs++] * sTexturePoolScaleT[sCurTex]);
        }
        else
        {
            *dst++ = 0;
            *dst++ = 0;
        }
        if(hasFog)
            vtxOffs += 4;
        if(color0Constant)
            vtxOffs += hasAlpha ? 4 : 3;
        if(hasColor)
        {
            *dst++ = buf_vbo[offset + vtxOffs++];
            *dst++ = buf_vbo[offset + vtxOffs++];
            *dst++ = buf_vbo[offset + vtxOffs++];
            if(hasAlpha)
                *dst++ = buf_vbo[offset + vtxOffs++];
            else
                *dst++ = 1.0f;
        }
        else
        {
            *dst++ = 1.0f;
            *dst++ = 1.0f;
            *dst++ = 1.0f;
            *dst++ = 1.0f;
        }
        
        offset += sVtxUnitSize;
    }

	C3D_DrawArrays(GPU_TRIANGLES, sBufIdx, buf_vbo_num_tris * 3);
    sBufIdx += buf_vbo_num_tris * 3;

    if(hasFog)
        renderFog(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
}

static void gfx_citro3d_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if(sBufIdx * 10 > 2 * 1024 * 1024 / 4)
    {
        printf("Poly buf over!\n");
        return;
    }

    /* Invariant: gfx_pc.c packed buf_vbo_num_tris * 3 vertices of exactly the current shader's
     * stride. A mismatch means the front end and this backend disagree on the vertex layout; reading
     * with the wrong stride walks off the buffer (unmapped reads, found by the tools/statediff tour in
     * the Treasure Chest Shop). Log it once with the details and skip the draw. */
    if (buf_vbo_len != buf_vbo_num_tris * 3 * (size_t)sVtxUnitSize) {
        static int sReported;
        if (sReported < 4) {
            extern void PortDbgX(const char* label, unsigned val);
            sReported++;
            PortDbgX("VTX-STRIDE MISMATCH shader_id", sShaderProgramPool[sCurShader].shader_id);
            PortDbgX("  cur shader slot", (unsigned)sCurShader);
            PortDbgX("  pool size", (unsigned)sShaderProgramPoolSize);
            PortDbgX("  backend stride", (unsigned)sVtxUnitSize);
            PortDbgX("  buf_vbo_len", (unsigned)buf_vbo_len);
            PortDbgX("  tris", (unsigned)buf_vbo_num_tris);
        }
        return;
    }
    applyAlphaTest();
    if(sShaderProgramPool[sCurShader].num_inputs >= 2)
    {
        renderTwoColorTris(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        return;
    }

    int offset = 0;
    float* dst = &((float*)sVboBuffer)[sBufIdx * 10];
    bool hasTex = sShaderProgramPool[sCurShader].used_textures[0] || sShaderProgramPool[sCurShader].used_textures[1];
    bool hasColor = sShaderProgramPool[sCurShader].num_inputs > 0;
    bool hasAlpha = (sShaderProgramPool[sCurShader].shader_id & SHADER_OPT_ALPHA) != 0;
    bool hasFog = (sShaderProgramPool[sCurShader].shader_id & SHADER_OPT_FOG) != 0;
    for(int i = 0; i < 3 * buf_vbo_num_tris; i++)
    {
        *dst++ = buf_vbo[offset + 1];
        *dst++ = -buf_vbo[offset + 0];
        *dst++ = -buf_vbo[offset + 2];
        *dst++ = buf_vbo[offset + 3];
        int vtxOffs = 4;
        if(hasTex)
        {
            *dst++ = buf_vbo[offset + vtxOffs++] * sTexturePoolScaleS[sCurTex];
            *dst++ = 1 - (buf_vbo[offset + vtxOffs++] * sTexturePoolScaleT[sCurTex]);
        }
        else
        {
            *dst++ = 0;
            *dst++ = 0;
        }
        if(hasFog)
            vtxOffs += 4;
        if(hasColor)
        {
            *dst++ = buf_vbo[offset + vtxOffs++];
            *dst++ = buf_vbo[offset + vtxOffs++];
            *dst++ = buf_vbo[offset + vtxOffs++];
            if(hasAlpha)
                *dst++ = buf_vbo[offset + vtxOffs++];
            else
                *dst++ = 1.0f;
        }
        else
        {
            *dst++ = 1.0f;
            *dst++ = 1.0f;
            *dst++ = 1.0f;
            *dst++ = 1.0f;
        }
        
        offset += sVtxUnitSize;
    }

	C3D_DrawArrays(GPU_TRIANGLES, sBufIdx, buf_vbo_num_tris * 3);
    sBufIdx += buf_vbo_num_tris * 3;

    if(hasFog)
        renderFog(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
}

static void gfx_citro3d_init(void)
{
    sVShaderDvlb = DVLB_ParseFile((u32*)shader_shbin, shader_shbin_size);
	shaderProgramInit(&sShaderProgram);
	shaderProgramSetVsh(&sShaderProgram, &sVShaderDvlb->DVLE[0]);
	C3D_BindProgram(&sShaderProgram);

	// Configure attributes for use with the vertex shader
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 4); // v0=position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v1=texcoord
	AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 4); // v2=color

	// Create the VBO (vertex buffer object)
	sVboBuffer = linearAlloc(2 * 1024 * 1024);

	// Configure buffers
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, sVboBuffer, 10 * 4, 3, 0x210);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(true, -1.0f, 0);
    C3D_DepthTest(false, GPU_LEQUAL, GPU_WRITE_ALL);
    C3D_AlphaTest(true, GPU_GREATER, 0x00);
}

static void gfx_citro3d_start_frame(void) {
    sBufIdx = 0;
}

struct GfxRenderingAPI gfx_citro3d_api = {
    gfx_citro3d_z_is_from_0_to_1,
    gfx_citro3d_unload_shader,
    gfx_citro3d_load_shader,
    gfx_citro3d_create_and_load_new_shader,
    gfx_citro3d_lookup_shader,
    gfx_citro3d_shader_get_info,
    gfx_citro3d_new_texture,
    gfx_citro3d_select_texture,
    gfx_citro3d_upload_texture,
    gfx_citro3d_set_sampler_parameters,
    gfx_citro3d_set_depth_test,
    gfx_citro3d_set_depth_mask,
    gfx_citro3d_set_zmode_decal,
    gfx_citro3d_set_viewport,
    gfx_citro3d_set_scissor,
    gfx_citro3d_set_use_alpha,
    gfx_citro3d_draw_triangles,
    gfx_citro3d_init,
    gfx_citro3d_start_frame
};

#endif

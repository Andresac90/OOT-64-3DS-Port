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

/* PORT (2026-09-25): N64 color combiner -> PICA TEV compiler.
 *
 * gfx_pc.c hands over the complete combiner (gfx_cc.h: both cycles, both channels, every input of
 * (A-B)*C+D). Each cycle/channel is lowered to 1..3 TEV operations (lowerChannel), RGB and alpha run
 * side by side in the same stages, cycle 1 follows cycle 0, and fog (blender cycle 1: lerp to the
 * fog color by shade alpha) is a last stage. Inputs:
 *   TEXEL0/1 -> GPU_TEXTURE0/1, SHADE -> GPU_PRIMARY_COLOR (the vertex color is always shade),
 *   PRIM, ENV, their alpha broadcasts, LOD fractions, ONE, ZERO, fog color -> per-stage CONSTANT
 *   (each stage has its own RGB and alpha constant, filled per draw from GfxCombineConsts),
 *   a second constant in one stage -> the TEV buffer's initial color (GPU_PREVIOUS_BUFFER),
 *   COMBINED -> GPU_PREVIOUS in cycle 1's first stage, else the TEV buffer (written by the last
 *   cycle-0 stage; PICA: stage s reads the buffer as updated by stages <= s-2).
 * Keys this can't express exactly are compiled approximately and reported once (PortDbgX). */

enum { KC_NONE, KC_ZERO, KC_ONE, KC_PRIM, KC_PRIMA, KC_ENV, KC_ENVA, KC_LODF, KC_PRIMLODF, KC_FOG };
enum { OK_CONST, OK_TEX0, OK_TEX1, OK_SHADE, OK_PREV, OK_COMB };

typedef struct {
    u8 kind, alpha, inv, k; /* alpha: use the source's alpha; inv: 1 - x; k: KC_* for OK_CONST */
} Opnd;

typedef struct {
    u8 func; /* GPU_COMBINEFUNC */
    Opnd src[3];
} ChanOp;

typedef struct {
    u8 func[2];     /* [channel] GPU_COMBINEFUNC */
    u8 src[2][3];   /* GPU_TEVSRC */
    u8 op[2][3];    /* GPU_TEVOP_RGB / GPU_TEVOP_A */
    u8 konst[2];    /* KC_* in this stage's CONSTANT, per channel */
} TevStage;

#define TEV_STAGES 6

struct ShaderProgram {
    uint64_t shader_id0;
    uint32_t shader_id1;
    bool used_textures[2];
    u8 num_stages;
    TevStage stages[TEV_STAGES];
    u8 buf_konst[2];  /* KC_* held in the TEV buffer's initial color, per channel */
    u8 buf_update[2]; /* TEV buffer update masks (stages 0..3), per channel */
};

/* OoT uses many distinct combiners (the sm64 [64] pool overflowed); lookups only happen when the key changes */
#define SHADER_POOL_CAP 1024
static struct ShaderProgram sShaderProgramPool[SHADER_POOL_CAP];
static uint16_t sShaderProgramPoolSize;

static int sCurShader = 0;
static struct GfxCombineConsts sConsts;

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

#define VTX_FLOATS 12 /* gfx_pc.c packs pos(4) uv0(2) uv1(2) shade(4); the VBO holds the same layout */

static bool sDepthTestOn = false;
static bool sDepthUpdateOn = true;
static bool sDepthDecal = false;

static bool sUseBlend;

static int sBufIdx = 0;

static void gfx_citro3d_unload_shader(struct ShaderProgram *old_prg) {

}

static Opnd opnd(u8 kind, u8 alpha, u8 k) {
    Opnd o = { kind, alpha, 0, k };
    return o;
}

static bool opndEq(Opnd a, Opnd b) {
    return a.kind == b.kind && a.alpha == b.alpha && a.inv == b.inv && a.k == b.k;
}

static bool isK(Opnd a, u8 k) {
    return a.kind == OK_CONST && !a.inv && a.k == k;
}

/* gfx_cc.h source -> operand, for channel ch (0 RGB, 1 alpha: every source is its alpha) */
static Opnd ccsOpnd(u8 s, int ch) {
    switch (s) {
        case CCS_1: return opnd(OK_CONST, ch, KC_ONE);
        case CCS_TEX0: return opnd(OK_TEX0, ch, 0);
        case CCS_TEX1: return opnd(OK_TEX1, ch, 0);
        case CCS_TEX0A: return opnd(OK_TEX0, 1, 0);
        case CCS_TEX1A: return opnd(OK_TEX1, 1, 0);
        case CCS_SHADE: return opnd(OK_SHADE, ch, 0);
        case CCS_SHADEA: return opnd(OK_SHADE, 1, 0);
        case CCS_PRIM: return opnd(OK_CONST, ch, KC_PRIM);
        case CCS_PRIMA: return opnd(OK_CONST, ch, ch ? KC_PRIM : KC_PRIMA);
        case CCS_ENV: return opnd(OK_CONST, ch, KC_ENV);
        case CCS_ENVA: return opnd(OK_CONST, ch, ch ? KC_ENV : KC_ENVA);
        case CCS_LODF: return opnd(OK_CONST, ch, KC_LODF);
        case CCS_PRIMLODF: return opnd(OK_CONST, ch, KC_PRIMLODF);
        case CCS_COMB: return opnd(OK_COMB, ch, 0);
        case CCS_COMBA: return opnd(OK_COMB, 1, 0);
    }
    return opnd(OK_CONST, ch, KC_ZERO);
}

static int emit(ChanOp* out, int n, u8 func, Opnd a, Opnd b, Opnd c) {
    out[n].func = func;
    out[n].src[0] = a;
    out[n].src[1] = b;
    out[n].src[2] = c;
    return n + 1;
}

/* (A-B)*C+D as TEV operations (PICA clamps every stage to 0..1). Returns the count (<= 3);
 * 0 = the channel passes the previous cycle through unchanged. */
static int lowerChannel(Opnd a, Opnd b, Opnd c, Opnd d, int ch, int cycle, ChanOp* out) {
    Opnd z = opnd(OK_CONST, ch, KC_ZERO), prev = opnd(OK_PREV, ch, 0);
    int n = 0;
    if (isK(c, KC_ZERO) || opndEq(a, b)) {
        if (cycle == 1 && d.kind == OK_COMB && d.alpha == ch) {
            return 0;
        }
        return emit(out, n, GPU_REPLACE, d, z, z);
    }
    if (isK(b, KC_ZERO)) {
        if (isK(d, KC_ZERO)) {
            if (isK(a, KC_ONE)) return emit(out, n, GPU_REPLACE, c, z, z);
            if (isK(c, KC_ONE)) return emit(out, n, GPU_REPLACE, a, z, z);
            return emit(out, n, GPU_MODULATE, a, c, z);
        }
        if (isK(a, KC_ONE)) return emit(out, n, GPU_ADD, c, d, z);
        if (isK(c, KC_ONE)) return emit(out, n, GPU_ADD, a, d, z);
        return emit(out, n, GPU_MULTIPLY_ADD, a, c, d);
    }
    if (opndEq(b, d)) {
        return emit(out, n, GPU_INTERPOLATE, a, b, c); /* a*c + b*(1-c) */
    }
    if (isK(a, KC_ONE) && b.kind != OK_PREV) { /* (1-B)*C+D */
        Opnd nb = b;
        nb.inv = 1;
        if (isK(d, KC_ZERO)) return emit(out, n, GPU_MODULATE, nb, c, z);
        if (isK(c, KC_ONE)) return emit(out, n, GPU_ADD, nb, d, z);
        return emit(out, n, GPU_MULTIPLY_ADD, nb, c, d);
    }
    if (isK(d, KC_ZERO)) { /* max(A-B, 0)*C is exact: C >= 0 and the N64 clamps the result */
        n = emit(out, n, GPU_SUBTRACT, a, b, z);
        if (isK(c, KC_ONE)) return n;
        return emit(out, n, GPU_MODULATE, prev, c, z);
    }
    if (isK(c, KC_ONE)) {
        n = emit(out, n, GPU_ADD, a, d, z);
        return emit(out, n, GPU_SUBTRACT, prev, b, z);
    }
    n = emit(out, n, GPU_INTERPOLATE, a, b, c); /* (A-B)*C + B */
    n = emit(out, n, GPU_ADD, prev, d, z);      /* ... + D */
    return emit(out, n, GPU_SUBTRACT, prev, b, z); /* ... - B */
}

/* compile state */
typedef struct {
    struct ShaderProgram* prg;
    bool exact;
    int cyc0Last;         /* last cycle-0 stage */
    bool prevIsComb[2];   /* per component (0 rgb, 1 alpha): PREVIOUS still holds cycle 0's output */
    bool bufComb[2];      /* buffer component carries cycle 0's output (updated by stage cyc0Last) */
    int bufKLast[2];      /* last stage reading the buffer's initial color */
} TevCompile;

static int arityOf(u8 func) {
    return func == GPU_REPLACE ? 1 : (func == GPU_INTERPOLATE || func == GPU_MULTIPLY_ADD) ? 3 : 2;
}

/* buffer component c as constant k at stage s: its initial color is visible until the cycle-0
 * output lands in it (PICA: stage s reads buffer updates of stages <= s-2) */
static bool bufKonstOk(TevCompile* tc, int c, u8 k, int s) {
    struct ShaderProgram* prg = tc->prg;
    if (prg->buf_konst[c] != KC_NONE && prg->buf_konst[c] != k) {
        return false;
    }
    if (tc->bufComb[c] && s > tc->cyc0Last + 1) {
        return false;
    }
    return true;
}

/* place op for channel ch into stage s; returns false (placing nothing) if it needs a second
 * constant with nowhere to put it: *needK then names that constant */
static bool placeOp(TevCompile* tc, TevStage* st, int s, int ch, int cycle, const ChanOp* op, u8* needK) {
    struct ShaderProgram* prg = tc->prg;
    int arity = arityOf(op->func);
    u8 konst = st->konst[ch], bufK = prg->buf_konst[ch];
    u8 src[3], ops[3];
    bool useBufK = false, useBufComb[2] = { false, false };
    for (int i = 0; i < 3; i++) {
        Opnd o = op->src[i];
        bool alpha = ch == 1 || o.alpha;
        if (i >= arity) { /* unused by the function: claim nothing */
            src[i] = GPU_PREVIOUS;
            ops[i] = ch ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
            continue;
        }
        if (o.kind == OK_COMB && cycle == 0) { /* cleared by gfx_cc_key; keep safe */
            o.kind = OK_CONST;
            o.k = KC_ZERO;
        }
        switch (o.kind) {
            case OK_TEX0: src[i] = GPU_TEXTURE0; break;
            case OK_TEX1: src[i] = GPU_TEXTURE1; break;
            case OK_SHADE: src[i] = GPU_PRIMARY_COLOR; break;
            case OK_PREV: src[i] = GPU_PREVIOUS; break;
            case OK_COMB:
                if (tc->prevIsComb[alpha ? 1 : 0]) {
                    src[i] = GPU_PREVIOUS;
                } else {
                    src[i] = GPU_PREVIOUS_BUFFER;
                    useBufComb[alpha ? 1 : 0] = true;
                }
                break;
            case OK_CONST:
                alpha = ch == 1; /* constants carry per-channel values (KC_PRIMA etc.) */
                if (konst == KC_NONE || konst == o.k) {
                    konst = o.k;
                    src[i] = GPU_CONSTANT;
                } else if ((bufK == KC_NONE || bufK == o.k) && bufKonstOk(tc, ch, o.k, s)) {
                    bufK = o.k;
                    useBufK = true;
                    src[i] = GPU_PREVIOUS_BUFFER;
                } else {
                    *needK = o.k;
                    return false;
                }
                break;
        }
        if (ch == 0) {
            ops[i] = alpha ? (o.inv ? GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_RGB_SRC_ALPHA)
                           : (o.inv ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR);
        } else {
            ops[i] = o.inv ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
        }
    }
    for (int c = 0; c < 2; c++) {
        if (useBufComb[c]) {
            if (tc->cyc0Last < 0 || tc->cyc0Last > 3 || s < tc->cyc0Last + 2 || tc->bufKLast[c] > tc->cyc0Last + 1) {
                tc->exact = false;
            } else {
                tc->bufComb[c] = true;
                prg->buf_update[c] = 1 << tc->cyc0Last;
            }
        }
    }
    if (useBufK) {
        prg->buf_konst[ch] = bufK;
        if (s > tc->bufKLast[ch]) {
            tc->bufKLast[ch] = s;
        }
    }
    st->konst[ch] = konst;
    st->func[ch] = op->func;
    for (int i = 0; i < 3; i++) {
        st->src[ch][i] = src[i];
        st->op[ch][i] = ops[i];
    }
    return true;
}

static ChanOp passOp(int ch) {
    ChanOp op;
    op.func = GPU_REPLACE;
    op.src[0] = opnd(OK_PREV, ch, 0);
    op.src[1] = op.src[2] = opnd(OK_CONST, ch, KC_ZERO);
    return op;
}

/* compile a key; returns false if some input had to be approximated */
static bool compileTev(struct ShaderProgram* prg) {
    uint64_t id0 = prg->shader_id0;
    TevCompile tc;
    int ns = 0;
    memset(&tc, 0, sizeof(tc));
    tc.prg = prg;
    tc.exact = true;
    tc.cyc0Last = -1;
    tc.bufKLast[0] = tc.bufKLast[1] = -1;
    memset(prg->stages, 0, sizeof(prg->stages));
    prg->buf_konst[0] = prg->buf_konst[1] = KC_NONE;
    prg->buf_update[0] = prg->buf_update[1] = 0;

    for (int cycle = 0; cycle < 2; cycle++) {
        ChanOp ops[2][4];
        int nops[2], next[2] = { 0, 0 };
        for (int ch = 0; ch < 2; ch++) {
            nops[ch] = lowerChannel(ccsOpnd(CC_SRC(id0, cycle, ch, 0), ch), ccsOpnd(CC_SRC(id0, cycle, ch, 1), ch),
                                    ccsOpnd(CC_SRC(id0, cycle, ch, 2), ch), ccsOpnd(CC_SRC(id0, cycle, ch, 3), ch), ch,
                                    cycle, ops[ch]);
        }
        if (cycle == 1) {
            tc.prevIsComb[0] = tc.prevIsComb[1] = true;
        }
        while (next[0] < nops[0] || next[1] < nops[1]) {
            if (ns >= TEV_STAGES) {
                tc.exact = false;
                goto done;
            }
            TevStage* st = &prg->stages[ns];
            bool changed[2] = { false, false };
            for (int ch = 0; ch < 2; ch++) {
                u8 needK = KC_NONE;
                if (next[ch] < nops[ch]) {
                    ChanOp* op = &ops[ch][next[ch]];
                    if (placeOp(&tc, st, ns, ch, cycle, op, &needK)) {
                        next[ch]++;
                        changed[ch] = true;
                        continue;
                    }
                    /* two constants: preload the second into PREVIOUS with this stage, if the op
                     * doesn't need PREVIOUS itself (COMBINED then comes from the buffer) */
                    bool usesPrev = false;
                    for (int i = 0; i < arityOf(op->func); i++) {
                        usesPrev |= op->src[i].kind == OK_PREV;
                    }
                    ChanOp pre;
                    pre.func = GPU_REPLACE;
                    pre.src[0] = opnd(OK_CONST, ch, needK);
                    pre.src[1] = pre.src[2] = pre.src[0];
                    if (!usesPrev && placeOp(&tc, st, ns, ch, cycle, &pre, &needK)) {
                        for (int i = 0; i < 3; i++) {
                            if (op->src[i].kind == OK_CONST && op->src[i].k == pre.src[0].k) {
                                u8 inv = op->src[i].inv;
                                op->src[i] = opnd(OK_PREV, ch, 0);
                                op->src[i].inv = inv;
                            }
                        }
                        changed[ch] = true;
                        continue;
                    }
                    tc.exact = false; /* approximate: drop the op's second constant */
                    for (int i = 0; i < 3; i++) {
                        if (op->src[i].kind == OK_CONST && op->src[i].k == needK) {
                            op->src[i].k = st->konst[ch];
                        }
                    }
                    if (placeOp(&tc, st, ns, ch, cycle, op, &needK)) {
                        next[ch]++;
                        changed[ch] = true;
                        continue;
                    }
                }
                ChanOp pass = passOp(ch);
                if (ns == 0) { /* nothing to pass through at stage 0 */
                    pass.src[0] = opnd(OK_CONST, ch, ch ? KC_ONE : KC_ZERO);
                }
                placeOp(&tc, st, ns, ch, cycle, &pass, &needK);
            }
            for (int ch = 0; ch < 2; ch++) {
                if (changed[ch]) {
                    tc.prevIsComb[ch] = false;
                }
            }
            ns++;
        }
        if (cycle == 0) {
            tc.cyc0Last = ns - 1;
        }
    }
done:
    if (prg->shader_id1 & SHADER_OPT_FOG) {
        if (ns < TEV_STAGES) {
            TevStage* st = &prg->stages[ns++];
            st->func[0] = GPU_INTERPOLATE; /* fog * f + prev * (1 - f), f = shade alpha */
            st->src[0][0] = GPU_CONSTANT;
            st->src[0][1] = GPU_PREVIOUS;
            st->src[0][2] = GPU_PRIMARY_COLOR;
            st->op[0][0] = GPU_TEVOP_RGB_SRC_COLOR;
            st->op[0][1] = GPU_TEVOP_RGB_SRC_COLOR;
            st->op[0][2] = GPU_TEVOP_RGB_SRC_ALPHA;
            st->konst[0] = KC_FOG;
            st->func[1] = GPU_REPLACE;
            st->src[1][0] = st->src[1][1] = st->src[1][2] = GPU_PREVIOUS;
            st->op[1][0] = st->op[1][1] = st->op[1][2] = GPU_TEVOP_A_SRC_ALPHA;
        } else {
            tc.exact = false;
        }
    }
    prg->num_stages = ns;
    return tc.exact;
}

/* value of constant k for channel ch, as 0..255 */
static u8 konstVal(u8 k, int ch, int comp) {
    const struct GfxCombineConsts* c = &sConsts;
    switch (k) {
        case KC_ONE: return 255;
        case KC_PRIM: return c->prim[ch ? 3 : comp];
        case KC_PRIMA: return c->prim[3];
        case KC_ENV: return c->env[ch ? 3 : comp];
        case KC_ENVA: return c->env[3];
        case KC_LODF: return c->lod_frac;
        case KC_PRIMLODF: return c->prim_lod_frac;
        case KC_FOG: return c->fog[ch ? 3 : comp];
    }
    return 0;
}

static u32 konstColor(u8 krgb, u8 ka) { /* PICA constant: 0xAABBGGRR */
    return konstVal(krgb, 0, 0) | (konstVal(krgb, 0, 1) << 8) | (konstVal(krgb, 0, 2) << 16) |
           ((u32)konstVal(ka, 1, 3) << 24);
}

/* program the TEV for the current shader and constants */
static void updateShader(void)
{
    if (sCurShader < 0 || sCurShader >= SHADER_POOL_CAP) {
        sCurShader = 0;
    }
    const struct ShaderProgram* prg = &sShaderProgramPool[sCurShader];
    for (int s = 0; s < TEV_STAGES; s++) {
        C3D_TexEnv* e = C3D_GetTexEnv(s);
        C3D_TexEnvInit(e);
        if (s >= prg->num_stages) {
            continue;
        }
        const TevStage* st = &prg->stages[s];
        C3D_TexEnvSrc(e, C3D_RGB, st->src[0][0], st->src[0][1], st->src[0][2]);
        C3D_TexEnvSrc(e, C3D_Alpha, st->src[1][0], st->src[1][1], st->src[1][2]);
        C3D_TexEnvOpRgb(e, st->op[0][0], st->op[0][1], st->op[0][2]);
        C3D_TexEnvOpAlpha(e, st->op[1][0], st->op[1][1], st->op[1][2]);
        C3D_TexEnvFunc(e, C3D_RGB, st->func[0]);
        C3D_TexEnvFunc(e, C3D_Alpha, st->func[1]);
        C3D_TexEnvColor(e, konstColor(st->konst[0], st->konst[1]));
    }
    C3D_TexEnvBufUpdate(C3D_RGB, prg->buf_update[0]);
    C3D_TexEnvBufUpdate(C3D_Alpha, prg->buf_update[1]);
    C3D_TexEnvBufColor(konstColor(prg->buf_konst[0], prg->buf_konst[1]));
}

static void gfx_citro3d_load_shader(struct ShaderProgram *new_prg) {
    sCurShader = new_prg - sShaderProgramPool;
    updateShader();
}

static void gfx_citro3d_set_combine_consts(const struct GfxCombineConsts *consts) {
    sConsts = *consts;
    updateShader();
}

static struct ShaderProgram *gfx_citro3d_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct ShaderProgram *prg;
    if (sShaderProgramPoolSize >= SHADER_POOL_CAP) {
        /* pool full: recycle the last slot (wrong-but-safe) */
        prg = &sShaderProgramPool[SHADER_POOL_CAP - 1];
    } else {
        prg = &sShaderProgramPool[sShaderProgramPoolSize++];
    }
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->used_textures[0] = (shader_id1 & SHADER_OPT_TEX0) != 0;
    prg->used_textures[1] = (shader_id1 & SHADER_OPT_TEX1) != 0;
    if (!compileTev(prg)) {
        static int sReported;
        if (sReported < 16) {
            extern void PortDbgX(const char* label, unsigned val);
            sReported++;
            PortDbgX("TEV approx key hi", (unsigned)(shader_id0 >> 32));
            PortDbgX("TEV approx key lo", (unsigned)shader_id0);
            PortDbgX("TEV approx opts", (unsigned)shader_id1);
        }
    }
    gfx_citro3d_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_citro3d_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    for (size_t i = 0; i < sShaderProgramPoolSize; i++) {
        if (sShaderProgramPool[i].shader_id0 == shader_id0 && sShaderProgramPool[i].shader_id1 == shader_id1) {
            return &sShaderProgramPool[i];
        }
    }
    return NULL;
}

static void gfx_citro3d_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = 1;
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

/* The top-screen target is PORTRAIT (240x400, rotated): x/y and width/height swap (sm64 3DS port),
 * and the vertex setup writes (y, -x), so N64 x runs down the target's second axis: a rectangle at x
 * sits at 400 - (x + width) there. PORT (2026-09-25): without that mirror every sub-viewport/scissor
 * was mirrored horizontally (full-screen ones are symmetric, so only small ones showed it: the A
 * button drawn over the hearts, found by tools/statediff fbdiff). */
#define TOP_W 400

extern int Port3ds_IsOffscreen(void);

static void gfx_citro3d_set_viewport(int x, int y, int width, int height) {
    if (Port3ds_IsOffscreen()) { /* 1x 320x240-space off-screen target (gfx_3ds.c) */
        C3D_SetViewport(y, 320 - (x + width), height, width);
        return;
    }
    x = TOP_W - (x + width);
    if (gGfx3DSMode == GFX_3DS_MODE_AA_22 || gGfx3DSMode == GFX_3DS_MODE_WIDE_AA_12)
        C3D_SetViewport(y * 2, x * 2, height * 2, width * 2);
    else if (gGfx3DSMode == GFX_3DS_MODE_WIDE)
        C3D_SetViewport(y, x * 2, height, width * 2);
    else
        C3D_SetViewport(y, x, height, width);
}

static void gfx_citro3d_set_scissor(int x, int y, int width, int height)
{
    if (Port3ds_IsOffscreen()) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, 320 - (x + width), y + height, 320 - x);
        return;
    }
    x = TOP_W - (x + width);
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
    u32 id = sShaderProgramPool[sCurShader].shader_id1;
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

/* tools/statediff per-draw attribution: gfx_pc.c sets an id per flush while the color readback runs;
 * every fragment that passes the depth test writes it into the (otherwise unused) stencil buffer. */
static int sDrawId;

void gfx_citro3d_set_draw_id(int id) {
    sDrawId = id;
}

static void applyDrawId(void) {
    if (sDrawId != 0) {
        C3D_StencilTest(true, GPU_ALWAYS, sDrawId, 0xFF, 0xFF);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    } else {
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
    }
}

static void gfx_citro3d_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (sBufIdx * VTX_FLOATS + buf_vbo_len > 2 * 1024 * 1024 / 4)
    {
        printf("Poly buf over!\n");
        return;
    }
    /* Invariant: gfx_pc.c packs VTX_FLOATS per vertex. A mismatch means the front end and this backend
     * disagree on the vertex layout; reading with the wrong stride walks off the buffer. */
    if (buf_vbo_len != buf_vbo_num_tris * 3 * VTX_FLOATS) {
        static int sReported;
        if (sReported < 4) {
            extern void PortDbgX(const char* label, unsigned val);
            sReported++;
            PortDbgX("VTX-STRIDE MISMATCH buf_vbo_len", (unsigned)buf_vbo_len);
            PortDbgX("  tris", (unsigned)buf_vbo_num_tris);
        }
        return;
    }
    applyAlphaTest();

    const struct ShaderProgram* prg = &sShaderProgramPool[sCurShader];
    /* texcoords are normalized to the uploaded texture; scale into the power-of-two PICA texture */
    float s0 = sTexturePoolScaleS[sTexUnits[0]], t0 = sTexturePoolScaleT[sTexUnits[0]];
    float s1 = sTexturePoolScaleS[sTexUnits[1]], t1 = sTexturePoolScaleT[sTexUnits[1]];
    const float* src = buf_vbo;
    float* dst = &((float*)sVboBuffer)[sBufIdx * VTX_FLOATS];
    for (size_t i = 0; i < 3 * buf_vbo_num_tris; i++, src += VTX_FLOATS)
    {
        *dst++ = src[1];
        *dst++ = -src[0];
        *dst++ = -src[2];
        *dst++ = src[3];
        *dst++ = src[4] * s0;
        *dst++ = 1 - (src[5] * t0);
        *dst++ = src[6] * s1;
        *dst++ = 1 - (src[7] * t1);
        *dst++ = src[8];
        *dst++ = src[9];
        *dst++ = src[10];
        *dst++ = src[11];
    }

    applyDrawId();
    C3D_DrawArrays(GPU_TRIANGLES, sBufIdx, buf_vbo_num_tris * 3);
    sBufIdx += buf_vbo_num_tris * 3;
    {
        extern u32 gPortPerfTris, gPortPerfDraws;
        gPortPerfTris += buf_vbo_num_tris;
        gPortPerfDraws++;
    }
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
	AttrInfo_AddLoader(attrInfo, 3, GPU_FLOAT, 2); // v3=texcoord1

	// Create the VBO (vertex buffer object)
	sVboBuffer = linearAlloc(2 * 1024 * 1024);

	// Configure buffers
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, sVboBuffer, VTX_FLOATS * 4, 4, 0x2310); // pos, uv0, uv1, color

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
    gfx_citro3d_set_combine_consts,
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

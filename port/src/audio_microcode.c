/*
 * audio_microcode.c - C reimplementation of the N64 audio RSP microcode (aspMain).
 * The ONE piece needed to complete audio. synthesis.c builds an Acmd list each
 * frame -> M_AUDTASK; on N64 the RSP executed it into PCM. Here on ARM11 CPU.
 * DMEM is big-endian throughout (heap data arrives BE via raw DMA); BE->LE only
 * at ndsp submit. SAFETY: bounds-checked, capped, cannot hang/OOB. Gated by
 * sPortAudioUcodeEnable (default 0). See PORT_ROADMAP.md section 8.3.
 */
#include "ultra64.h"
#include "ultra64/abi.h"

extern void PortDbgX(const char* label, unsigned val);

int sPortAudioUcodeEnable = 1; /* PORT (2026-09-21): audio data tables + seq->font map fixed;
                                * engine now produces real PCM (nonzero/full-scale verified),
                                * so submit mixed frames to ndsp. */
extern void Port3ds_AudioSubmitFrame(const s16* be_stereo, int nsamples);

#define DMEM_BYTES 0x2000
#define DMEM_MASK  (DMEM_BYTES - 1)
static u8 sDmem[DMEM_BYTES];

static s16 be16_read(const u8* p) { return (s16)((p[0] << 8) | p[1]); }
static void be16_write(u8* p, s32 v) {
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    p[0] = (u8)((v >> 8) & 0xFF); p[1] = (u8)(v & 0xFF);
}
static s16 dmem_r(u32 off, int i) { return be16_read(&sDmem[(off + (u32)(i * 2)) & DMEM_MASK]); }
static void dmem_w(u32 off, int i, s32 v) { be16_write(&sDmem[(off + (u32)(i * 2)) & DMEM_MASK], v); }

static u8* ext_ptr(u32 addr) {
    addr &= 0x7FFFFFFFu;
    /* Accept any plausible 3DS RAM address. The audio heap/buffers live in the
     * app's static/low memory (~0x001xxxxx), NOT only the 0x08000000 malloc
     * region, so the old >=0x08000000 gate rejected every valid audio address
     * (loads, saves, codebooks) and the interpreter did nothing. */
    if (addr < 0x00100000u) return NULL;
    return (u8*)(uintptr_t)addr;
}
static s32 clamp16(s32 v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

static u16 sIn, sOut, sCount;
static u16 sAdpcmPredCount;
static const u8* sLoopState;
static s16 sAdpcmBook[8 * 2 * 8];
static s32 sEnvTgtL, sEnvTgtR;

static void op_clearbuff(u32 dmem, u32 count) {
    u32 i; count = (count + 15) & ~15u;
    for (i = 0; i < count; i++) sDmem[(dmem + i) & DMEM_MASK] = 0;
}
static void op_dmemmove(u32 in, u32 out, u32 count) {
    u32 i; for (i = 0; i < count; i++) sDmem[(out + i) & DMEM_MASK] = sDmem[(in + i) & DMEM_MASK];
}
static void op_loadbuff(u32 addr, u32 dmem, u32 count) {
    const u8* s = ext_ptr(addr); u32 i;
    if (s == NULL) return;
    count &= ~15u;
    for (i = 0; i < count; i++) sDmem[(dmem + i) & DMEM_MASK] = s[i];
}
static void op_savebuff(u32 dmem, u32 addr, u32 count) {
    u8* d = ext_ptr(addr); u32 i;
    if (d == NULL) return;
    count &= ~15u;
    for (i = 0; i < count; i++) d[i] = sDmem[(dmem + i) & DMEM_MASK];
}
static void op_interleave(u32 out, u32 inL, u32 inR, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        be16_write(&sDmem[(out + i * 4 + 0) & DMEM_MASK], dmem_r(inL, (int)i));
        be16_write(&sDmem[(out + i * 4 + 2) & DMEM_MASK], dmem_r(inR, (int)i));
    }
}
static void op_mixer(u32 in, u32 out, s16 gain, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        s32 acc = (s32)dmem_r(out, (int)i) + (((s32)dmem_r(in, (int)i) * gain) >> 15);
        dmem_w(out, (int)i, clamp16(acc));
    }
}
static void op_loadadpcm(u32 addr, u32 count) {
    const u8* s = ext_ptr(addr); u32 nshorts, i;
    sAdpcmPredCount = (u16)(count / 32);
    if (s == NULL) { sAdpcmPredCount = 0; return; }
    nshorts = count / 2;
    if (nshorts > sizeof(sAdpcmBook) / sizeof(sAdpcmBook[0])) nshorts = sizeof(sAdpcmBook) / sizeof(sAdpcmBook[0]);
    for (i = 0; i < nshorts; i++) sAdpcmBook[i] = be16_read(&s[i * 2]);
}
static void op_setloop(u32 addr) { sLoopState = ext_ptr(addr); }

static void op_adpcm(u32 flags) {
    s16 hist[2]; u32 nframes, f, inoff, outoff; int i;
    hist[0] = 0; hist[1] = 0;
    if (!(flags & A_INIT) && sLoopState) { hist[0] = be16_read(&sLoopState[0]); hist[1] = be16_read(&sLoopState[2]); }
    nframes = (sCount + 15) / 16;
    inoff = sIn; outoff = sOut;
    for (f = 0; f < nframes; f++) {
        u8 hdr = sDmem[inoff & DMEM_MASK]; int scale, pred; const s16* book; s32 samples[16];
        inoff++;
        scale = hdr >> 4; pred = hdr & 0xF;
        if (pred >= sAdpcmPredCount) pred = 0;
        book = &sAdpcmBook[pred * 16];
        for (i = 0; i < 16; i += 2) {
            u8 b = sDmem[inoff & DMEM_MASK]; int n0, n1;
            inoff++;
            n0 = (s8)(b & 0xF0) >> 4; n1 = (s8)(b << 4) >> 4;
            samples[i] = (s32)n0 << scale; samples[i + 1] = (s32)n1 << scale;
        }
        for (i = 0; i < 16; i++) {
            s32 acc = (book[i & 7] * hist[0] + book[8 + (i & 7)] * hist[1]) >> 11; s32 v;
            acc += samples[i]; v = clamp16(acc);
            dmem_w(outoff, i, v);
            hist[1] = hist[0]; hist[0] = (s16)v;
        }
        outoff += 32;
    }
    if (sLoopState) { u8* ls = (u8*)sLoopState; be16_write(&ls[0], hist[0]); be16_write(&ls[2], hist[1]); }
}
static void op_resample(u32 flags, u32 pitch, u32 stateAddr) {
    u8* st = ext_ptr(stateAddr); u32 pos = 0, step; u32 i;
    if (st && !(flags & A_INIT)) pos = ((u32)(u16)be16_read(&st[0]) << 16) | (u16)be16_read(&st[2]);
    step = pitch << 1;
    for (i = 0; i < sCount; i++) {
        u32 idx = pos >> 16; s32 frac = pos & 0xFFFF;
        s32 a = dmem_r(sIn, (int)idx); s32 b = dmem_r(sIn, (int)idx + 1);
        s32 v = a + (((b - a) * frac) >> 16);
        dmem_w(sOut, (int)i, clamp16(v));
        pos += step;
    }
    if (st) { be16_write(&st[0], (s16)(pos >> 16)); be16_write(&st[2], (s16)(pos & 0xFFFF)); }
}
static void op_envsetup2(u32 volL, u32 volR) { sEnvTgtL = (s16)volL; sEnvTgtR = (s16)volR; }
static void op_envmixer(void) {
    u32 i;
    for (i = 0; i < sCount; i++) {
        s32 s = dmem_r(sIn, (int)i);
        s32 l = dmem_r(sOut, (int)i) + ((s * sEnvTgtL) >> 15);
        dmem_w(sOut, (int)i, clamp16(l));
    }
}

/* Validation: every 128 audio tasks, log the final PCM buffer stats to boot.log.
 * Confirms the interpreter produces real signal without needing audio capture. */
static void log_pcm_stats(const u8* buf, u32 bytes) {
    static u32 calls = 0;
    u32 nsamp, i, nz = 0; s32 mn = 32767, mx = -32768;
    calls++;
    if ((calls & 127) != 1) return;
    nsamp = bytes / 2;
    for (i = 0; i < nsamp; i++) {
        s32 v = be16_read(&buf[i * 2]);
        if (v != 0) nz++;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    PortDbgX("[audio] nsamp", nsamp);
    PortDbgX("[audio] nonzero", nz);
    PortDbgX("[audio] min(u16)", (unsigned)(mn & 0xFFFF));
    PortDbgX("[audio] max(u16)", (unsigned)(mx & 0xFFFF));
}

void PortAudio_RunTask(OSTask* task) {
    const Acmd* cmd; u32 n, k;
    u8* lastAiBuf = NULL; u32 lastAiBytes = 0;
    if (task == NULL || task->t.data_ptr == NULL) return;
    cmd = (const Acmd*)task->t.data_ptr;
    n = (u32)(task->t.data_size / sizeof(Acmd));
    if (n > 8192) n = 8192;
    { static unsigned rc = 0; if ((++rc & 127) == 1) PortDbgX("[audio] RUNTASK n", n); }
    for (k = 0; k < n; k++) {
        u32 w0 = cmd[k].words.w0, w1 = cmd[k].words.w1, op = w0 >> 24;
        switch (op) {
            case A_SPNOOP: break;
            case A_CLEARBUFF: op_clearbuff(w0 & 0xFFFF, w1); break;
            case A_DMEMMOVE:  op_dmemmove(w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF); break;
            case A_LOADBUFF:  op_loadbuff(w1, w0 & 0xFFFF, ((w0 >> 16) & 0xFF) << 4); break;
            case A_SAVEBUFF:
                op_savebuff(w0 & 0xFFFF, w1, ((w0 >> 16) & 0xFF) << 4);
                lastAiBuf = ext_ptr(w1); lastAiBytes = ((w0 >> 16) & 0xFF) << 4;
                break;
            case A_SETBUFF:   sIn = w0 & 0xFFFF; sOut = w1 >> 16; sCount = w1 & 0xFFFF; break;
            case A_INTERLEAVE: op_interleave(w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF, (((w0 >> 16) & 0xFF) << 4) >> 2); break;
            case A_MIXER:     op_mixer(w1 >> 16, w1 & 0xFFFF, (s16)(w0 & 0xFFFF), ((w0 >> 16) & 0xFF) << 4); break;
            case A_LOADADPCM: op_loadadpcm(w1, w0 & 0xFFFFFF); break;
            case A_SETLOOP:   op_setloop(w1); break;
            case A_ADPCM:     op_adpcm((w0 >> 16) & 0xFF); break;
            case A_RESAMPLE:  op_resample((w0 >> 16) & 0xFF, w0 & 0xFFFF, w1); break;
            case A_ENVSETUP2: op_envsetup2(w1 >> 16, w1 & 0xFFFF); break;
            case A_ENVMIXER:  op_envmixer(); break;
            default: break;
        }
    }
    { static unsigned lc = 0; if ((++lc & 127) == 1) PortDbgX("[audio] lastAiBytes", lastAiBytes); }
    if (lastAiBuf && lastAiBytes >= 4) {
        log_pcm_stats(lastAiBuf, lastAiBytes);
        if (sPortAudioUcodeEnable) Port3ds_AudioSubmitFrame((const s16*)lastAiBuf, (int)(lastAiBytes / 4));
    }
}

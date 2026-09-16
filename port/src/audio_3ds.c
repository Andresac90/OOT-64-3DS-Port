/*
 * audio_3ds.c — M3a: ndsp output plumbing (see PORT_ROADMAP.md §8.2).
 * One stereo PCM16 channel at 32000 Hz (the OoT engine rate; ndsp resamples
 * internally). Provides:
 *   Port3ds_AudioInit()   — boot init + 0.3 s proof-of-life tone
 *   Port3ds_AudioSubmit() — future M3c/M3d sink: interleaved s16 LR frames
 *   Port3ds_AudioReady()  — whether ndsp is up
 *
 * ndsp needs the DSP firmware dump (sdmc:/3ds/dspfirm.cdc). If it is missing we
 * skip init entirely and the game runs silent exactly as before — this file must
 * never be able to break boot.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define PORT_AUDIO_RATE   32000
#define PORT_AUDIO_NBUFS  3
#define PORT_AUDIO_MAXSAMPLES 1600 /* per submit; engine frames are ~544-736 */

static bool sNdspOk = false;
static ndspWaveBuf sWaveBufs[PORT_AUDIO_NBUFS];
static s16* sWaveData[PORT_AUDIO_NBUFS];
static int sNextBuf = 0;

static ndspWaveBuf sBeepBuf;
static s16* sBeepData;

void Port3ds_AudioInit(void) {
    /* Guard: without the DSP firmware dump, do not even attempt ndspInit. */
    FILE* f = fopen("sdmc:/3ds/dspfirm.cdc", "rb");
    if (f == NULL) {
        printf("[audio] no sdmc:/3ds/dspfirm.cdc - audio disabled\n");
        return;
    }
    fclose(f);

    if (R_FAILED(ndspInit())) {
        printf("[audio] ndspInit failed - audio disabled\n");
        return;
    }
    sNdspOk = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)PORT_AUDIO_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    for (int i = 0; i < PORT_AUDIO_NBUFS; i++) {
        sWaveData[i] = (s16*)linearAlloc(PORT_AUDIO_MAXSAMPLES * 2 * sizeof(s16));
        memset(&sWaveBufs[i], 0, sizeof(ndspWaveBuf));
        sWaveBufs[i].status = NDSP_WBUF_DONE;
    }

    /* Proof-of-life: 0.3 s 440 Hz tone so the user can hear the pipe works. */
    {
        int n = PORT_AUDIO_RATE * 3 / 10;
        sBeepData = (s16*)linearAlloc(n * 2 * sizeof(s16));
        if (sBeepData != NULL) {
            for (int i = 0; i < n; i++) {
                float env = (i < n - 800) ? 1.0f : (float)(n - i) / 800.0f;
                s16 v = (s16)(2500.0f * env * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)PORT_AUDIO_RATE));
                sBeepData[2 * i] = v;
                sBeepData[2 * i + 1] = v;
            }
            memset(&sBeepBuf, 0, sizeof(sBeepBuf));
            sBeepBuf.data_vaddr = sBeepData;
            sBeepBuf.nsamples = n;
            DSP_FlushDataCache(sBeepData, n * 2 * sizeof(s16));
            ndspChnWaveBufAdd(0, &sBeepBuf);
        }
    }
    printf("[audio] ndsp up @32000Hz (beep = pipe OK)\n");
}

int Port3ds_AudioReady(void) {
    return sNdspOk ? 1 : 0;
}

/* M3c/M3d sink: push one engine frame of interleaved stereo s16. Drops the frame
 * if all wave bufs are still queued (underrun-safe, never blocks). */
void Port3ds_AudioSubmit(const s16* samples, int nsamples) {
    if (!sNdspOk || samples == NULL || nsamples <= 0) {
        return;
    }
    if (nsamples > PORT_AUDIO_MAXSAMPLES) {
        nsamples = PORT_AUDIO_MAXSAMPLES;
    }
    ndspWaveBuf* wb = &sWaveBufs[sNextBuf];
    if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) {
        return; /* all buffers in flight; drop */
    }
    memcpy(sWaveData[sNextBuf], samples, (size_t)nsamples * 2 * sizeof(s16));
    memset(wb, 0, sizeof(ndspWaveBuf));
    wb->data_vaddr = sWaveData[sNextBuf];
    wb->nsamples = (u32)nsamples;
    DSP_FlushDataCache(sWaveData[sNextBuf], (size_t)nsamples * 2 * sizeof(s16));
    ndspChnWaveBufAdd(0, wb);
    sNextBuf = (sNextBuf + 1) % PORT_AUDIO_NBUFS;
}

/* Microcode sink: the interpreter produces big-endian interleaved stereo s16
 * (the N64 DMEM representation). Byte-swap to little-endian for ndsp, then reuse
 * the wave-buf submit path above. */
void Port3ds_AudioSubmitFrame(const s16* be_stereo, int nsamples) {
    static s16 le[PORT_AUDIO_MAXSAMPLES * 2];
    const u8* src;
    int n2, i;
    if (!sNdspOk || be_stereo == NULL || nsamples <= 0) {
        return;
    }
    if (nsamples > PORT_AUDIO_MAXSAMPLES) {
        nsamples = PORT_AUDIO_MAXSAMPLES;
    }
    src = (const u8*)be_stereo;
    n2 = nsamples * 2; /* L+R */
    for (i = 0; i < n2; i++) {
        le[i] = (s16)((src[i * 2] << 8) | src[i * 2 + 1]);
    }
    Port3ds_AudioSubmit(le, nsamples);
}

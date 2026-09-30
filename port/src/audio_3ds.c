/*
 * audio_3ds.c — M3a: ndsp output plumbing (see PORT_ROADMAP.md §8.2).
 * One stereo PCM16 channel at 32000 Hz (the OoT engine rate; ndsp resamples
 * internally). Provides:
 *   Port3ds_AudioInit()   — boot init
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
#define PORT_AUDIO_NBUFS  4
#define PORT_AUDIO_MAXSAMPLES 1600 /* per submit; engine frames are ~544-736 */

static bool sNdspOk = false;
static ndspWaveBuf sWaveBufs[PORT_AUDIO_NBUFS];
static s16* sWaveData[PORT_AUDIO_NBUFS];
static int sNextBuf = 0;


void Port3ds_AudioInit(void) {
    extern void PortDbg(const char*);
    /* PORT (2026-09-21): try ndspInit even without sdmc:/3ds/dspfirm.cdc — Azahar/Citra
     * HLE-emulate the DSP and do not need the firmware dump. If ndspInit fails we still
     * disable gracefully (never break boot). On real hardware the dump is required. */
    { FILE* f = fopen("sdmc:/3ds/dspfirm.cdc", "rb");
      if (f) { fclose(f); PortDbg("[audio] dspfirm.cdc present"); }
      else PortDbg("[audio] no dspfirm.cdc - trying ndsp HLE anyway"); }

    if (R_FAILED(ndspInit())) {
        PortDbg("[audio] ndspInit FAILED - audio disabled");
        return;
    }
    PortDbg("[audio] ndspInit OK - ndsp up");
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

    printf("[audio] ndsp up @32000Hz\n");
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

/* PORT (2026-09-21): capture the engine's mixed PCM to a WAV on the SD card, so the audio
 * can be VERIFIED/heard even when ndsp is unavailable (Azahar needs dspfirm.cdc, which ndspInit
 * requires). Writes 20 s of 32 kHz stereo s16 (from the first audible frame) to sdmc:/3ds/oot/oot_audio.wav then stops. */
static void wav_put32(FILE* f, unsigned v) { fputc(v&0xFF,f);fputc((v>>8)&0xFF,f);fputc((v>>16)&0xFF,f);fputc((v>>24)&0xFF,f); }
static void wav_put16(FILE* f, unsigned v) { fputc(v&0xFF,f);fputc((v>>8)&0xFF,f); }
static FILE* sWavFile = NULL;
static unsigned sWavSamples = 0;
static int sWavDone = 0;
#define WAV_MAX_SAMPLES (32000u * 20u)
static void Port3ds_AudioDumpWav(const s16* le_stereo, int nsamples) {
    extern void PortDbg(const char*);
    static int sWavWanted = -1;
    int i;
    if (sWavDone) return;
    if (sWavWanted < 0) {
        /* opt-in (verification builds/emulator): writing to the SD card from the frame loop stalls real
         * hardware for the whole capture, so only capture when sdmc:/3ds/oot/capture_audio exists */
        FILE* flag = fopen("sdmc:/3ds/oot/capture_audio", "rb");
        sWavWanted = flag != NULL;
        if (flag != NULL) fclose(flag);
    }
    if (!sWavWanted) {
        sWavDone = 1;
        return;
    }
    if (sWavFile == NULL) {
        /* start at the first audible frame (the boot logo is silent, as on N64) */
        for (i = 0; i < nsamples * 2 && le_stereo[i] == 0; i++) {}
        if (i == nsamples * 2) return;
        sWavFile = fopen("sdmc:/3ds/oot/oot_audio.wav", "wb");
        if (sWavFile == NULL) { sWavDone = 1; return; }
        for (i = 0; i < 44; i++) fputc(0, sWavFile); /* header placeholder */
        PortDbg("[audio] WAV capture started -> sdmc:/3ds/oot/oot_audio.wav");
    }
    fwrite(le_stereo, 4, (size_t)nsamples, sWavFile); /* native LE s16 stereo == WAV PCM layout */
    sWavSamples += (unsigned)nsamples;
    if (sWavSamples >= WAV_MAX_SAMPLES) {
        unsigned dataBytes = sWavSamples * 4;
        fseek(sWavFile, 0, SEEK_SET);
        fputs("RIFF", sWavFile); wav_put32(sWavFile, 36 + dataBytes); fputs("WAVE", sWavFile);
        fputs("fmt ", sWavFile); wav_put32(sWavFile, 16); wav_put16(sWavFile, 1); wav_put16(sWavFile, 2);
        wav_put32(sWavFile, 32000); wav_put32(sWavFile, 32000 * 4); wav_put16(sWavFile, 4); wav_put16(sWavFile, 16);
        fputs("data", sWavFile); wav_put32(sWavFile, dataBytes);
        fclose(sWavFile); sWavFile = NULL; sWavDone = 1;
        PortDbg("[audio] WAV capture COMPLETE (20s) -> sdmc:/3ds/oot/oot_audio.wav");
    }
}

/* AI sink, called from osAiSetNextBuffer with each finished engine audio frame (native s16
 * interleaved stereo, like the N64 AI DMA): capture to WAV, then queue on ndsp. */
void Port3ds_AudioSubmitAi(const s16* stereo, int nframes) {
    if (stereo == NULL || nframes <= 0) {
        return;
    }
    if (nframes > PORT_AUDIO_MAXSAMPLES) {
        nframes = PORT_AUDIO_MAXSAMPLES;
    }
    Port3ds_AudioDumpWav(stereo, nframes); /* capture regardless of ndsp availability */
    Port3ds_AudioSubmit(stereo, nframes);
}

/* Stereo frames queued on ndsp and not yet played (the N64 osAiGetLength equivalent). */
int Port3ds_AudioQueuedFrames(void) {
    int i, queued = 0;
    if (!sNdspOk) {
        return 0;
    }
    for (i = 0; i < PORT_AUDIO_NBUFS; i++) {
        if (sWaveBufs[i].status == NDSP_WBUF_QUEUED || sWaveBufs[i].status == NDSP_WBUF_PLAYING) {
            queued += (int)sWaveBufs[i].nsamples;
        }
    }
    if (queued > 0) {
        queued -= (int)ndspChnGetSamplePos(0); /* position within the buffer now playing */
    }
    return queued > 0 ? queued : 0;
}

/* PORT (2026-09-29): the audio "RSP". On the N64 the RSP runs each audio task while the CPU builds the next
 * one (AudioMgr_HandleRetrace dispatches task N, runs AudioThread_Update for N+1, then waits for N-1); the
 * engine's DMA buffer lifetimes assume that overlap. Here the C microcode (PortAudio_RunTask) runs on a
 * worker thread on another core - core 2 on New 3DS, the system core 1 (with an app CPU-time allowance) on
 * Old 3DS - so it no longer eats the main thread's frame time. Dispatching a task first waits for the
 * previous one, so tasks still run in order. If no worker can be created, tasks run synchronously. */
/* OSTask (ultra64.h, which clashes with <3ds.h>) copied as opaque bytes; sched_shim.c checks the size */
#define PORT_OSTASK_SIZE 64
typedef struct { u8 bytes[PORT_OSTASK_SIZE]; } PortOSTask;
extern void PortAudio_RunTask(void* task);

static Thread sAudioWorker;
static LightEvent sAudioJobStart, sAudioJobDone;
static PortOSTask sAudioJob __attribute__((aligned(8)));
static volatile bool sAudioJobBusy;
static int sAudioAsync = -1; /* -1 = not tried yet */

u64 gPortPerfAudioUcode, gPortPerfAudioWait; /* ticks: microcode (worker core), main thread waiting for it */
u32 gPortPerfAudioTasks;

static void Port3ds_AudioWorkerMain(void* arg) {
    (void)arg;
    for (;;) {
        u64 t0;
        LightEvent_Wait(&sAudioJobStart);
        t0 = svcGetSystemTick();
        PortAudio_RunTask(&sAudioJob);
        gPortPerfAudioUcode += svcGetSystemTick() - t0;
        gPortPerfAudioTasks++;
        sAudioJobBusy = false;
        LightEvent_Signal(&sAudioJobDone);
    }
}

static void Port3ds_AudioWorkerStart(void) {
    extern void PortDbg(const char*);
    bool n3ds = false;
    s32 prio = 0x30;
    int core;

    sAudioAsync = 0;
    LightEvent_Init(&sAudioJobStart, RESET_ONESHOT);
    LightEvent_Init(&sAudioJobDone, RESET_ONESHOT);
    APT_CheckNew3DS(&n3ds);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    core = n3ds ? 2 : 1;
    if (!n3ds) {
        /* Old 3DS: the audio microcode needs ~33% of the system core (measured: 5.3-6.0 ms per task at
         * 268 MHz, 60 tasks/s). Stock firmware allows at most 30%; Luma3DS allows up to 89%, but 80%
         * has been reported to hard-lock when opening the Rosalina menu. Ask for 55%, else 30%. */
        if (R_FAILED(APT_SetAppCpuTimeLimit(55))) {
            APT_SetAppCpuTimeLimit(30);
        }
    }
    sAudioWorker = threadCreate(Port3ds_AudioWorkerMain, NULL, 16 * 1024, prio > 0x18 ? prio - 1 : prio, core, true);
    if (sAudioWorker == NULL && n3ds) {
        APT_SetAppCpuTimeLimit(30);
        core = 1;
        sAudioWorker = threadCreate(Port3ds_AudioWorkerMain, NULL, 16 * 1024, prio > 0x18 ? prio - 1 : prio, core, true);
    }
    if (sAudioWorker != NULL) {
        sAudioAsync = 1;
        PortDbg(core == 2 ? "[audio] microcode worker on core 2" : "[audio] microcode worker on core 1");
    } else {
        PortDbg("[audio] no microcode worker thread - running audio tasks synchronously");
    }
}

/* wait until the previous audio task has finished */
void Port3ds_AudioTaskWait(void) {
    u64 t0 = svcGetSystemTick();
    while (sAudioJobBusy) {
        LightEvent_Wait(&sAudioJobDone);
    }
    gPortPerfAudioWait += svcGetSystemTick() - t0;
}

/* run (or start) one audio task; returns once it may be treated as complete by the scheduler */
void Port3ds_AudioTaskRun(void* task) {
    if (sAudioAsync < 0) {
        Port3ds_AudioWorkerStart();
    }
    if (sAudioAsync == 0) {
        u64 t0 = svcGetSystemTick();
        PortAudio_RunTask(task);
        gPortPerfAudioUcode += svcGetSystemTick() - t0;
        gPortPerfAudioTasks++;
        return;
    }
    Port3ds_AudioTaskWait();
    memcpy(&sAudioJob, task, sizeof(sAudioJob)); /* the command list stays valid: the engine double-buffers it */
    sAudioJobBusy = true;
    LightEvent_Signal(&sAudioJobStart);
}

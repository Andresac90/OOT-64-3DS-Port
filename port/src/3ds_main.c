#include <malloc.h>
/*
 * 3ds_main.c — Nintendo 3DS entry point (libctru). Replaces pc_main.c/pc_gfx.c.
 * Boots the port runtime, drives the OoT gamestate loop, and reads the real
 * 3DS buttons into the controller shim. Rendering goes through the citro3d
 * backend (gfx3ds/gfx_citro3d.c) via the same gfx_pc interface proven on PC.
 */
#include <3ds.h>
#include <stdio.h>
#include <dirent.h>

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

static void Port3ds_PollInput(void) {
    hidScanInput();
    u32 k = hidKeysHeld();
    unsigned short b = 0;
    if (k & KEY_A)      b |= BTN_A_;
    if (k & KEY_B)      b |= BTN_B_;
    if (k & KEY_X)      b |= BTN_CUP_;   /* map X/Y to C-up/down for now */
    if (k & KEY_Y)      b |= BTN_CDOWN_;
    if (k & KEY_START)  b |= BTN_START_;
    if (k & KEY_L)      b |= BTN_L_;
    if (k & KEY_R)      b |= BTN_R_;
    if (k & KEY_ZL)     b |= BTN_Z_;     /* New 3DS ZL as Z trigger */
    if (k & KEY_ZR)     b |= BTN_Z_;
    if (k & KEY_DUP)    b |= BTN_DUP_;
    if (k & KEY_DDOWN)  b |= BTN_DDOWN_;
    if (k & KEY_DLEFT)  b |= BTN_DLEFT_;
    if (k & KEY_DRIGHT) b |= BTN_DRIGHT_;
    /* C-stick (New 3DS) -> C buttons */
    if (k & KEY_CSTICK_UP)    b |= BTN_CUP_;
    if (k & KEY_CSTICK_DOWN)  b |= BTN_CDOWN_;
    if (k & KEY_CSTICK_LEFT)  b |= BTN_CLEFT_;
    if (k & KEY_CSTICK_RIGHT) b |= BTN_CRIGHT_;

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
    printf("[gfx] citro3d renderer initialized\n");
}

void PortGfx_FrameReady(void) {}

/* PORT (2026-09-24): N64-faithful pacing. OoT advances its logic once every R_UPDATE_RATE VI
 * retraces (3 -> 20 updates/s) and the N64 AudioMgr runs on EVERY retrace (60/s), independent of
 * the game. The port used to run one update per rendered frame (measured 26-29/s -> game ~1.4x too
 * fast, speed varying with scene load) and pumped audio once per update. Here: after presenting,
 * wait whole retraces until R_UPDATE_RATE retrace periods have passed since the previous update,
 * pumping audio once per retrace. A frame that renders late simply runs late (like N64 lag). */
static unsigned sPortAudioPumps = 0;
static void Port3ds_PaceFrame(void) {
    extern void* gRegEditor;
    extern void Port3ds_PumpAudio(void);
    static u64 sLast = 0;
    const double kRetraceMs = 1000.0 / 59.83; /* 3DS LCD refresh */
    int rate = 3;
    if (gRegEditor) rate = *(short*)((char*)gRegEditor + 0x14 + 126 * 2); /* R_UPDATE_RATE = SREG(30) */
    if (rate < 1) rate = 1;
    if (rate > 6) rate = 6;
    int pumped = 0;
    u64 now;
    for (;;) {
        gspWaitForVBlank();
        Port3ds_PumpAudio(); /* build+dispatch one audio RSP task per retrace, as on N64 */
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
    }
    sPortAudioPumps += (unsigned)pumped;
    sLast = now;
}

/* PORT PERF (2026-09-28): per-frame CPU breakdown in 268 MHz system ticks, logged every 300 frames as
 * average microseconds per frame: game = everything between two graph tasks (game logic + DL build),
 * dl = display-list interpretation (gfx_run), swap = frame end/GPU wait, pace = retrace waits + audio;
 * plus triangles and draw calls sent to the GPU per frame (gfx_pc.c counters). */
u32 gPortPerfTris, gPortPerfDraws;
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
        PortDbgX("perf tex imports/frame", gPortPerfTexImports / frames);
        gPortPerfTex = gPortPerfVtx = gPortPerfTri = gPortPerfFlush = 0;
        gPortPerfTexImports = 0;
    }
    PortDbgX("perf us/frame swap", (unsigned)(sPerfSwap / div));
    PortDbgX("perf us/frame pace", (unsigned)(sPerfPace / div));
    PortDbgX("perf tris/frame", gPortPerfTris / frames);
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
    sPerfGame = sPerfDl = sPerfSwap = sPerfPace = 0;
    gPortPerfTris = gPortPerfDraws = 0;
}

void PortGfx_RunTask(OSTask* task) {
    u64 tA = svcGetSystemTick(), tB, tC, tD;
    if (sPerfLastEnd != 0) sPerfGame += tA - sPerfLastEnd;
    if (!sGfxInited) PortGfx_Init();
    Port3ds_PollInput();
    { extern void Port3ds_PumpInput(void); Port3ds_PumpInput(); } /* live buttons -> game PadMgr */
    { extern void Audio_PortEnsureNullChannels(void); Audio_PortEnsureNullChannels(); } /* keep uninit audio channels non-NULL so direct game audio calls don't crash */
    gfx_start_frame();
    gfx_run((Gfx*)task->t.data_ptr);
    tB = svcGetSystemTick();
    gfx_end_frame();
    tC = svcGetSystemTick();
    Port3ds_PaceFrame();
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
          if (gRegEditor) PortDbgX("perf R_UPDATE_RATE", (unsigned)*(short*)((char*)gRegEditor + 0x14 + 126 * 2));
          PortDbgX("perf audio pumps/s x10", (unsigned)((u64)sPortAudioPumps * 10000ull / (t1 - t0 ? t1 - t0 : 1)));
          sPortAudioPumps = 0;
          Port3ds_PerfReport(n);
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
    printf("%s\n", s);
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
unsigned PortMem_ReadableEnd(unsigned addr) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return 0;
    if (mi.state == MEMSTATE_FREE || mi.state == MEMSTATE_RESERVED) return 0;
    if (!(mi.perm & MEMPERM_READ)) return 0;
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

    Graph_ThreadEntry(0);

    boot_halt("graph loop exited");
    gfxExit();
    return 0;
}

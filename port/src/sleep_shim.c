/*
 * sleep_shim.c — replaces src/libc64/sleep.c. The original sleeps by waiting
 * on an OS timer message; here the host just sleeps.
 */
#include "ultra64.h"

extern int usleep();

#define CYCLES_PER_USEC 47 /* ~46.875MHz N64 counter */

#ifdef __3DS__
/* PORT (2026-10-13): on the N64 the audio thread keeps running while the game thread sleeps. The pause screen
 * opening sleeps 50 ms (KaleidoScope_ProcessPlayerPreRender, waiting for the RDP) and some transitions 100 ms
 * (z_fbdemo.c): with one thread that was 50-100 ms without audio, a click each time the pause screen opened
 * (hardware boot.log gaps, 2026-10-13). Sleep in short steps and pump audio on each retrace (3ds_main.c). */
static void Port_Sleep(u32 usec) {
    extern void Port3ds_PumpAudioInLoad(void);
    while (usec > 0) {
        u32 step = usec > 2000 ? 2000 : usec;
        usleep(step);
        usec -= step;
        Port3ds_PumpAudioInLoad();
    }
}
#define usleep(us) Port_Sleep(us)
#endif

void Sleep_Cycles(u64 cycles) { usleep((u32)(cycles / CYCLES_PER_USEC)); }
void Sleep_Nsec(u32 nsec) { usleep(nsec / 1000); }
void Sleep_Usec(u32 usec) { usleep(usec); }
void Sleep_Msec(u32 ms) { usleep(ms * 1000); }
void Sleep_Sec(u32 sec) { usleep(sec * 1000000); }

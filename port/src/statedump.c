/* statedump.c - write raw game state to the SD card for tools/statediff (N64-vs-3DS state diff).
 * Enabled only in builds with GAME_EXTRA=-DPORT_STATEDUMP=<gameplayFrames>; the hook in
 * Play_Update calls these once, at the start of that frame. Files (all raw, native byte order):
 *   sdmc:/3ds/oot/sd_play.bin    PlayState
 *   sdmc:/3ds/oot/sd_save.bin    SaveContext
 *   sdmc:/3ds/oot/sd_actors.bin  per actor: u32 category, u32 size, then the Actor bytes
 * tools/statediff/statediff.py reads the same structs from ares (N64) and diffs field by field. */
#include <stdio.h>
#include <stdint.h>

static FILE* sActors;

static void write_file(const char* path, const void* data, unsigned size) {
    FILE* f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}

void PortStateDump_Begin(const void* play, unsigned playSize, const void* save, unsigned saveSize) {
    write_file("sdmc:/3ds/oot/sd_play.bin", play, playSize);
    write_file("sdmc:/3ds/oot/sd_save.bin", save, saveSize);
    sActors = fopen("sdmc:/3ds/oot/sd_actors.bin", "wb");
}

void PortStateDump_Actor(unsigned category, const void* actor, unsigned size) {
    uint32_t hdr[2] = { category, size };
    if (sActors != NULL) {
        fwrite(hdr, 1, sizeof(hdr), sActors);
        fwrite(actor, 1, size, sActors);
    }
}

void PortStateDump_End(void) {
    extern void PortDbg(const char* str);
    if (sActors != NULL) {
        fclose(sActors);
        sActors = NULL;
    }
    PortDbg("statedump: wrote sd_play/sd_save/sd_actors.bin");
}

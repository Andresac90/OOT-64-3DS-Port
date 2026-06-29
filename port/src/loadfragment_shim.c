/*
 * loadfragment_shim.c — replaces src/libu64/loadfragment2_n64.c.
 * All overlay code is statically linked native. Returning vramStart as the
 * "loaded" address makes every relocation offset in the engine compute to
 * zero, so the tables' native function pointers pass through unchanged.
 */
#include "ultra64.h"

void Overlay_Relocate(void* allocatedRamAddr, void* ovlRelocs, void* vramStart) {
    (void)allocatedRamAddr; (void)ovlRelocs; (void)vramStart;
}

u32 Overlay_Load(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd,
                 void* allocatedRamAddr) {
    (void)vromStart; (void)vromEnd; (void)allocatedRamAddr;
    return (u32)((uintptr_t)vramEnd - (uintptr_t)vramStart);
}

void* Overlay_AllocateAndLoad(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd) {
    (void)vromStart; (void)vromEnd; (void)vramEnd;
    return vramStart;
}

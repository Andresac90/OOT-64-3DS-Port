#include "alignment.h"
#include "buffers.h"
#include "gfx.h"
#include "ultra64.h"

ALIGNED(16) u64 gGfxSPTaskOutputBuffer[0x3000];

ALIGNED(16) u64 gGfxSPTaskYieldBuffer[OS_YIELD_DATA_SIZE / sizeof(u64)];

ALIGNED(16) u64 gGfxSPTaskStack[SP_DRAM_STACK_SIZE64];

ALIGNED(16) GfxPool gGfxPools[2];

#ifdef __3DS__
/* PORT: frame interpolation (gfx_pc.c interp_key) tells per-frame DLs from static ones by this range */
const unsigned gPortGfxPoolsSize = sizeof(gGfxPools);
#endif

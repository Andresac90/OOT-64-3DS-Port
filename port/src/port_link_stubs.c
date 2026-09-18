/* PORT (2026-09-17): n64dd is disabled in this port (idle.c D_80121211 == 0), but the
 * game references these n64dd entry points. Provide inert stubs so the image links;
 * none run because the n64dd path is gated off. Signatures are loose on purpose (C
 * links by name); real bodies live in src/n64dd/ if n64dd is ever brought up. */
void func_801C6EA0() {}
void func_801C70FC() {}
void func_801C7268() {}
void func_801C7658() {}
void func_801C7818() {}
void func_801C7C1C() {}
void func_801C7E78() {}
void n64dd_SetDiskVersion() {}

/* _Printf is libultra's printf formatter (sprintf/osSyncPrintf backend). The port logs
 * via PortDbg, so a no-op formatter is enough to link and boot silently. Revisit if any
 * gameplay path needs real sprintf output. */
int _Printf() { return 0; }

/* PORT DEBUG (2026-09-17): the 42MB resident .data (CodeSet) leaves little app memory.
 * If the CCI ExHeader only grants O3DS 64MB, the default 24MB heap + 32MB linear heap
 * won't fit and gfxInitDefault's framebuffer lands on unmapped linear memory (the
 * 0x10000000 crash). Shrink the heaps to fit-test the OOM theory (override libctru's
 * weak __ctru_*_size). Restore/raise once the memory grant is confirmed. */
unsigned __ctru_heap_size = 6u * 1024 * 1024;
unsigned __ctru_linear_heap_size = 8u * 1024 * 1024;

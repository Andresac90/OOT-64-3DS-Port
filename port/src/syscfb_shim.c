/*
 * syscfb_shim.c — replaces src/code/sys_cfb.c for the port.
 * The N64 placed framebuffers at fixed RAM addresses (~0x803c0000). On PC that
 * worked via the 0x80000000 mmap; on 3DS those addresses are invalid. Here we
 * allocate real memory for the two framebuffers so any direct framebuffer
 * access (PreRender, framebuffer effects) writes to valid RAM. The actual
 * display goes through the gfx renderer (GL/citro3d), not these buffers.
 */
#include "ultra64.h"
#include "sys_cfb.h"

extern void* malloc(unsigned long);

uintptr_t sSysCfbFbPtr[2];
uintptr_t sSysCfbEnd;

#define CFB_W 320
#define CFB_H 240

void SysCfb_Init(s32 n64dd) {
    (void)n64dd;
    /* one 16bpp-sized buffer is screenSize*2; allocate generously (32bpp) */
    u32 bytes = CFB_W * CFB_H * 4;
    sSysCfbFbPtr[0] = (uintptr_t)malloc(bytes);
    sSysCfbFbPtr[1] = (uintptr_t)malloc(bytes);
    sSysCfbEnd = sSysCfbFbPtr[1] + bytes;
}

void SysCfb_Reset(void) {
    sSysCfbFbPtr[0] = 0;
    sSysCfbFbPtr[1] = 0;
}

void* SysCfb_GetFbPtr(s32 idx) {
    return (void*)sSysCfbFbPtr[idx & 1];
}

void* SysCfb_GetFbEnd(void) {
    return (void*)sSysCfbEnd;
}

u32 SysCfb_GetFbSize(void) {
    return CFB_W * CFB_H * 4;
}

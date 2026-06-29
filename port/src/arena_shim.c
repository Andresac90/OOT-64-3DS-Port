/*
 * arena_shim.c — replaces src/libc64/malloc.c (SystemArena).
 * Host malloc with a live-pointer registry: frees of pointers we never
 * allocated (e.g. the overlay loader's identity "allocations") are ignored,
 * and the N64's 8MB heap limit disappears.
 */
#include "ultra64.h"

extern void* malloc();
extern void* calloc();
extern void* realloc();
extern void free();
extern int fprintf();
extern void* stderr;
extern void* memset();

#define REG_BITS 16
#define REG_SIZE (1 << REG_BITS)
#define DELETED ((void*)-1)
static void* sLive[REG_SIZE];
static u32 sLiveCount = 0;

static u32 RegHash(void* p) {
    return (((u32)(uintptr_t)p) >> 3) * 2654435761u >> (32 - REG_BITS);
}

static void RegInsert(void* p) {
    u32 i = RegHash(p);
    while (sLive[i] != 0 && sLive[i] != DELETED) {
        i = (i + 1) & (REG_SIZE - 1);
    }
    sLive[i] = p;
    sLiveCount++;
}

static s32 RegRemove(void* p) {
    u32 i = RegHash(p);
    u32 n;
    for (n = 0; n < REG_SIZE; n++) {
        if (sLive[i] == 0) {
            return 0;
        }
        if (sLive[i] == p) {
            sLive[i] = DELETED;
            sLiveCount--;
            return 1;
        }
        i = (i + 1) & (REG_SIZE - 1);
    }
    return 0;
}

void* SystemArena_Malloc(u32 size) {
    void* p = malloc(size ? size : 1);
    if (p != 0) RegInsert(p);
    return p;
}
void* SystemArena_MallocDebug(u32 size, const char* file, int line) {
    (void)file; (void)line;
    return SystemArena_Malloc(size);
}
void* SystemArena_MallocR(u32 size) { return SystemArena_Malloc(size); }
void* SystemArena_MallocRDebug(u32 size, const char* file, int line) {
    (void)file; (void)line;
    return SystemArena_Malloc(size);
}

void* SystemArena_Realloc(void* ptr, u32 newSize) {
    void* q;
    if (ptr == 0) {
        return SystemArena_Malloc(newSize);
    }
    if (!RegRemove(ptr)) {
        fprintf(stderr, "[arena] realloc of foreign ptr %p — fresh alloc\n", ptr);
        return SystemArena_Malloc(newSize);
    }
    q = realloc(ptr, newSize ? newSize : 1);
    if (q != 0) RegInsert(q);
    return q;
}
void* SystemArena_ReallocDebug(void* ptr, u32 newSize, const char* file, int line) {
    (void)file; (void)line;
    return SystemArena_Realloc(ptr, newSize);
}

void SystemArena_Free(void* ptr) {
    if (ptr == 0) return;
    if (RegRemove(ptr)) {
        free(ptr);
    } else {
        fprintf(stderr, "[arena] free of foreign ptr %p ignored\n", ptr);
    }
}
void SystemArena_FreeDebug(void* ptr, const char* file, int line) {
    (void)file; (void)line;
    SystemArena_Free(ptr);
}

void* SystemArena_Calloc(u32 num, u32 size) {
    void* p = SystemArena_Malloc(num * size);
    if (p != 0) memset(p, 0, num * size);
    return p;
}

void SystemArena_Display(void) {}
void SystemArena_GetSizes(u32* outMaxFree, u32* outFree, u32* outAlloc) {
    if (outMaxFree != 0) *outMaxFree = 64 * 1024 * 1024;
    if (outFree != 0) *outFree = 64 * 1024 * 1024;
    if (outAlloc != 0) *outAlloc = sLiveCount * 16;
}
void SystemArena_Check(void) {}
void SystemArena_CheckPointer(void* ptr, u32 size, const char* name, const char* action) {
    (void)ptr; (void)size; (void)name; (void)action;
}
void SystemArena_Init(void* start, u32 size) { (void)start; (void)size; }
void SystemArena_Cleanup(void) {}
s32 SystemArena_IsInitialized(void) { return 1; }

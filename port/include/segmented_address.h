#ifndef SEGMENTED_ADDRESS_H
#define SEGMENTED_ADDRESS_H

/* PORT OVERRIDE of include/segmented_address.h.
 * Natively-linked asset data stores real pointers where the N64 stored
 * segment offsets; both flow through here. Real pointers (>= 0x10000000)
 * pass through untouched; true segment references resolve via gSegments,
 * adding K0BASE only when the base is an emulated-RDRAM physical address.
 */

#include "ultra64.h"
#include "stdint.h"

extern uintptr_t gSegments[NUM_SEGMENTS];

static inline void* PortSegmentedToVirtual(uintptr_t addr) {
    uintptr_t v;

#ifdef __3DS__
    /* 3DS: host RAM is at physical-equivalent addresses below 0x80000000; there
     * is no emulated RDRAM at 0x80000000. KSEG pointers strip to host physical;
     * native pointers pass through; segment refs resolve via gSegments (host). */
    if (addr >= 0x80000000u) {
        return (void*)(addr & 0x1FFFFFFFu); /* KSEG0/KSEG1 -> host physical */
    }
    if (addr >= 0x10000000u) {
        return (void*)addr; /* native host pointer */
    }
    /* If this "segment" slot is unset, `addr` is NOT a segment offset — it is a
     * real pointer baked into the native asset that just happens to land below
     * 0x10000000 (3DS .data lives there). Pass it through untouched. Mangling it
     * as a segment offset (dropping the high nibble) was corrupting real asset
     * pointers like 0x01f71a80 -> 0x00f71a80. */
    if (gSegments[SEGMENT_NUMBER(addr)] == 0) {
        return (void*)addr;
    }
    v = gSegments[SEGMENT_NUMBER(addr)] + SEGMENT_OFFSET(addr);
    if (v >= 0x80000000u) {
        v &= 0x1FFFFFFFu; /* segment base was a KSEG0 address */
    }
    return (void*)v;
#else
    if (addr >= 0x10000000u) {
        return (void*)addr; /* already a native (or KSEG) pointer */
    }
    v = gSegments[SEGMENT_NUMBER(addr)] + SEGMENT_OFFSET(addr);
    if (v < 0x10000000u) {
        v += K0BASE; /* physical emulated-RDRAM base: restore KSEG0 view */
    }
    return (void*)v;
#endif
}

#define SEGMENTED_TO_VIRTUAL(addr) PortSegmentedToVirtual((uintptr_t)(addr))

#endif

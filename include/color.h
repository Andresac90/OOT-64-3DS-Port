#ifndef COLOR_H
#define COLOR_H

#include "ultra64/ultratypes.h"

typedef struct Color_RGB8 {
    u8 r, g, b;
} Color_RGB8;

typedef struct Color_RGBA8 {
    u8 r, g, b, a;
} Color_RGBA8;

// only use when necessary for alignment purposes
typedef union Color_RGBA8_u32 {
    struct {
#ifdef __3DS__
        // PORT: the game writes .rgba (0xRRGGBBAA) and reads .r/.g/.b/.a, and vice versa; on
        // little-endian the u32's low byte comes first, so the byte fields are declared reversed
        u8 a, b, g, r;
#else
        u8 r, g, b, a;
#endif
    };
    u32 rgba;
} Color_RGBA8_u32;

typedef struct Color_RGBAf {
    f32 r, g, b, a;
} Color_RGBAf;

typedef union Color_RGBA16 {
    struct {
        u16 r : 5;
        u16 g : 5;
        u16 b : 5;
        u16 a : 1;
    };
    u16 rgba;
} Color_RGBA16;

#define RGBA8(r, g, b, a) ((((r) & 0xFF) << 24) | (((g) & 0xFF) << 16) | (((b) & 0xFF) << 8) | (((a) & 0xFF) << 0))

#endif

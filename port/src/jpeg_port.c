/*
 * jpeg_port.c — pre-rendered room backgrounds (JPEG) for the 3DS port.
 *
 * On N64, Room_DecodeJpeg (z_room.c) recognises a JFIF image by its first word, Huffman-decodes it on
 * the CPU, runs the IDCT/colour conversion as an RSP task (njpgdspMain), and copies the resulting
 * 320x240 RGBA5551 image back over the JPEG data IN PLACE; the S2DEX background draw then reads it.
 * In the port the JPEG sits in a natively-compiled u64 array (bytes swizzled per 8 on the LE ARM11),
 * so the N64 marker check never matched and no RSP JPEG task exists. This decodes the same file with
 * stb_image (MIT/public domain) and writes the image back in the same swizzled u64 layout every other
 * compiled texture has, so the standard texture path draws it unchanged. A second visit finds no
 * marker and skips decoding, like the original.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#include "third_party/stb_image.h"

#define BG_WIDTH 320
#define BG_HEIGHT 240
#define BG_BYTES (BG_WIDTH * BG_HEIGHT * 2) /* the JPEG array and the decoded image share this size */

/* logical (N64 big-endian order) byte i of a natively-compiled u64 array starting at 8-aligned base */

extern void gfx_texture_cache_invalidate_range(const void* start, uint32_t size);

static uint8_t swz_get(const void* base, uint32_t i) {
    return *(const uint8_t*)(((uintptr_t)base + i) ^ 7u);
}
static void swz_put(void* base, uint32_t i, uint8_t v) {
    *(uint8_t*)(((uintptr_t)base + i) ^ 7u) = v;
}

/* Returns 1 if a JPEG was decoded (in place), 0 if the data was not a JPEG (e.g. already decoded). */
int PortJpeg_DecodeRoomImageInPlace(void* data) {
    uint8_t* jpg;
    unsigned char* rgb;
    int w = 0, h = 0, comp = 0, x, y;

    if (data == NULL || ((uintptr_t)data & 7) != 0) return 0;
    if (swz_get(data, 0) != 0xFF || swz_get(data, 1) != 0xD8 || swz_get(data, 2) != 0xFF || swz_get(data, 3) != 0xE0)
        return 0;

    jpg = (uint8_t*)malloc(BG_BYTES);
    if (jpg == NULL) return 0;
    for (x = 0; x < BG_BYTES; x++) jpg[x] = swz_get(data, (uint32_t)x);
    rgb = stbi_load_from_memory(jpg, BG_BYTES, &w, &h, &comp, 3);
    free(jpg);
    if (rgb == NULL) return 0;

    for (y = 0; y < BG_HEIGHT; y++) {
        for (x = 0; x < BG_WIDTH; x++) {
            uint16_t v = 0;
            if (x < w && y < h) {
                const unsigned char* p = rgb + ((size_t)y * w + x) * 3;
                v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | 1);
            }
            swz_put(data, (uint32_t)(y * BG_WIDTH + x) * 2, (uint8_t)(v >> 8));
            swz_put(data, (uint32_t)(y * BG_WIDTH + x) * 2 + 1, (uint8_t)(v & 0xFF));
        }
    }
    stbi_image_free(rgb);
    gfx_texture_cache_invalidate_range(data, BG_BYTES);
    return 1;
}

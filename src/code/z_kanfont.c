#include "kanread.h"
#include "message_data_static.h"
#include "printf.h"
#include "segment_symbols.h"
#include "translation.h"
#include "versions.h"
#include "dma.h"
#include "font.h"
#include "message.h"

/**
 * Loads a texture from kanji for the requested `character` into the character texture buffer
 * at `codePointIndex`. The value of `character` is the SHIFT-JIS encoding of the character.
 */
void Font_LoadCharWide(Font* font, u16 character, u16 codePointIndex) {
#if OOT_NTSC
    DMA_REQUEST_SYNC(&font->charTexBuf[codePointIndex],
                     (uintptr_t)_kanjiSegmentRomStart + Kanji_OffsetFromShiftJIS(character), FONT_CHAR_TEX_SIZE,
                     "../z_kanfont.c", UNK_LINE);
#endif
}

/**
 * Loads a texture from nes_font_static for the requested `character` into the character texture buffer
 * at `codePointIndex`. The value of `character` is the ASCII codepoint subtract ' '/0x20.
 */
void Font_LoadChar(Font* font, u8 character, u16 codePointIndex) {
    s32 offset = character * FONT_CHAR_TEX_SIZE;

    DMA_REQUEST_SYNC(&font->charTexBuf[codePointIndex], (uintptr_t)_nes_font_staticSegmentRomStart + offset,
                     FONT_CHAR_TEX_SIZE, "../z_kanfont.c", 93);
}

#if PLATFORM_IQUE
void Font_LoadCharCHN(Font* font, u16 character, u16 codePointIndex) {
    s32 offset = character * FONT_CHAR_TEX_SIZE;

    DMA_REQUEST_SYNC(&font->charTexBuf[codePointIndex], (uintptr_t)_nes_font_staticSegmentRomStart + offset,
                     FONT_CHAR_TEX_SIZE, "../z_kanfont.c", UNK_LINE);
}
#endif

/**
 * Loads a message box icon from message_static, such as the ending triangle/square or choice arrow into the
 * icon buffer.
 * The different icons are given in the MessageBoxIcon enum.
 */
void Font_LoadMessageBoxIcon(Font* font, u16 icon) {
    DMA_REQUEST_SYNC(font->iconBuf,
                     (uintptr_t)_message_staticSegmentRomStart + 4 * MESSAGE_STATIC_TEX_SIZE +
                         icon * FONT_CHAR_TEX_SIZE,
                     FONT_CHAR_TEX_SIZE, "../z_kanfont.c", 100);
}

/**
 * Loads a full set of character textures based on their ordering in the message with text id 0xFFFC into
 * the font buffer.
 */
void Font_LoadOrderedFont(Font* font) {
    s32 size;
    s32 len;
    s32 codePointIndex;
    s32 fontBufIndex;
    u32 offset;
    const char* messageDataStart;
    u16* msgBufWide;

#if OOT_NTSC && !PLATFORM_IQUE
#ifdef __3DS__
    /* PORT: On N64 the _message_0xXXXX_jpn markers are contiguous, in message-index
     * order, inside one ROM segment, so `_message_0xFFFD_jpn - _message_0xFFFC_jpn`
     * is message 0xFFFC's byte length and a single segment-offset DMA loads it.
     * In this native little-endian build each _message_*_jpn[] is a separately
     * linked, scattered array, which breaks that in two ways:
     *   (1) the marker subtraction is meaningless (here it comes out to -4, i.e.
     *       size 0xfffffffc -> a ~4GB DMA), and
     *   (2) wide chars are stored big-endian (ARG2 emits hi,lo), so reading them
     *       as native u16 byteswaps every value, including the 0x8170 terminator,
     *       and the glyph loop never ends -> fontBuf overrun / crash.
     * Read message 0xFFFC straight from its native array, byteswapping BE->native
     * and bounding to msgBufWide. */
    {
        const u8* p = (const u8*)_message_0xFFFC_jpn;
        s32 n;
        for (n = 0; n < (s32)(sizeof(font->msgBufWide) / sizeof(font->msgBufWide[0])) - 1; n++) {
            u16 w = (u16)((p[n * 2] << 8) | p[n * 2 + 1]);
            font->msgBufWide[n] = w;
            if (w == MESSAGE_WIDE_END) {
                break;
            }
        }
        font->msgBufWide[n] = MESSAGE_WIDE_END;
        font->msgOffset = 0;
        size = font->msgLength = n * 2;
        len = n;
    }
#else
    messageDataStart = (const char*)_jpn_message_data_staticSegmentStart;
    font->msgOffset = _message_0xFFFC_jpn - messageDataStart;
    size = font->msgLength = _message_0xFFFD_jpn - _message_0xFFFC_jpn;
    len = (u32)size / 2;
    DMA_REQUEST_SYNC(font->msgBufWide, (uintptr_t)_jpn_message_data_staticSegmentRomStart + font->msgOffset, size,
                     "../z_kanfont.c", UNK_LINE);
#endif

    PRINTF("msg_data=%x,  msg_data0=%x   jj=%x\n", font->msgOffset, font->msgLength, len);

    fontBufIndex = 0;
    for (codePointIndex = 0; font->msgBufWide[codePointIndex] != MESSAGE_WIDE_END; codePointIndex++) {
        if (len < codePointIndex) {
            PRINTF(T("ＥＲＲＯＲ！！  エラー！！！  error───！！！！\n", "ERROR!!  Error!!!  error───!!!!\n"));
            return;
        }

        if (font->msgBufWide[codePointIndex] != MESSAGE_WIDE_NEWLINE) {
#ifdef __3DS__
            /* never let the glyph loop run past fontBuf (320 chars) */
            if ((u32)(fontBufIndex * 8) + FONT_CHAR_TEX_SIZE > sizeof(font->fontBuf)) {
                break;
            }
#endif
            offset = Kanji_OffsetFromShiftJIS(font->msgBufWide[codePointIndex]);
            DMA_REQUEST_SYNC(&font->fontBuf[fontBufIndex * 8], (uintptr_t)_kanjiSegmentRomStart + offset,
                             FONT_CHAR_TEX_SIZE, "../z_kanfont.c", UNK_LINE);
            fontBufIndex += FONT_CHAR_TEX_SIZE / 8;
        }
    }
#elif OOT_PAL
    messageDataStart = (const char*)_nes_message_data_staticSegmentStart;
    font->msgOffset = _message_0xFFFC_nes - messageDataStart;
    size = font->msgLength = _message_0xFFFD_nes - _message_0xFFFC_nes;
    len = size;
    DMA_REQUEST_SYNC(font->msgBuf, (uintptr_t)_nes_message_data_staticSegmentRomStart + font->msgOffset, len,
                     "../z_kanfont.c", 122);

    PRINTF("msg_data=%x,  msg_data0=%x   jj=%x\n", font->msgOffset, font->msgLength, len * 1);

    fontBufIndex = 0;
    for (codePointIndex = 0; font->msgBuf[codePointIndex] != MESSAGE_END; codePointIndex++) {
        if (codePointIndex > (len * 1)) {
            PRINTF(T("ＥＲＲＯＲ！！  エラー！！！  error───！！！！\n", "ERROR!!  Error!!!  error───!!!!\n"));
            return;
        }

        if (font->msgBuf[codePointIndex] != MESSAGE_NEWLINE) {
            PRINTF("nes_mes_buf[%d]=%d\n", codePointIndex, font->msgBuf[codePointIndex]);

            offset = (font->msgBuf[codePointIndex] - ' ') * FONT_CHAR_TEX_SIZE;
            DMA_REQUEST_SYNC(font->fontBuf + fontBufIndex * 8, (uintptr_t)_nes_font_staticSegmentRomStart + offset,
                             FONT_CHAR_TEX_SIZE, "../z_kanfont.c", 134);
            fontBufIndex += FONT_CHAR_TEX_SIZE / 8;
        }
    }
#elif PLATFORM_IQUE
    messageDataStart = (const char*)_jpn_message_data_staticSegmentStart;
    font->msgOffset = _message_0xFFFC_jpn - messageDataStart;
    size = font->msgLength = _message_0xFFFD_jpn - _message_0xFFFC_jpn;
    len = (u32)size / 2;
    DMA_REQUEST_SYNC(font->msgBufWide, (uintptr_t)_jpn_message_data_staticSegmentRomStart + font->msgOffset, size,
                     "../z_kanfont.c", UNK_LINE);

    PRINTF("msg_data=%x,  msg_data0=%x   jj=%x\n", font->msgOffset, font->msgLength, len);

    // Workaround for EGCS internal compiler error (see docs/compilers.md)
    msgBufWide = font->msgBufWide;
    fontBufIndex = 0;
    for (codePointIndex = 0; msgBufWide[codePointIndex] != MESSAGE_WIDE_END; codePointIndex++) {
        if (len < codePointIndex) {
            PRINTF(T("ＥＲＲＯＲ！！  エラー！！！  error───！！！！\n", "ERROR!!  Error!!!  error───!!!!\n"));
            return;
        }

        if (msgBufWide[codePointIndex] != MESSAGE_WIDE_NEWLINE) {
            offset = Kanji_OffsetFromShiftJIS(msgBufWide[codePointIndex]);
            DMA_REQUEST_SYNC(&font->fontBuf[fontBufIndex * 8], (uintptr_t)_kanjiSegmentRomStart + offset,
                             FONT_CHAR_TEX_SIZE, "../z_kanfont.c", UNK_LINE);
            fontBufIndex += FONT_CHAR_TEX_SIZE / 8;
        }
    }
#endif
}

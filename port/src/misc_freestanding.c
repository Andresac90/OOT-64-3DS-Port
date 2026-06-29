/*
 * misc_freestanding.c — symbols whose declared types clash with simple
 * definitions; compiled without any decomp headers on purpose.
 */

/* IEEE-754 quiet NaN bit pattern; declared f32 in gu.h, bits are bits. */
unsigned int __libm_qnan_f = 0x7FBFFFFF;

/* JIS kanji glyph offset lookup (libultra kanread, never decompiled).
 * JPN-text rendering will be wrong until implemented; EN text unaffected. */
int Kanji_OffsetFromShiftJIS(int sjis) {
    (void)sjis;
    return 0;
}

// SPDX-License-Identifier: MIT
#ifndef ROMM_FONT_H
#define ROMM_FONT_H

#include <stdint.h>

// 4-bit anti-aliased bitmap font (tools/mkfont.py). Glyph rows are packed two
// pixels per byte (low nibble first), each row padded to a whole byte.
typedef struct
{
    uint16_t cp;      // Unicode code point (table sorted by it)
    uint8_t w, h;     // Bitmap size
    int8_t xoff;      // From the pen position
    int8_t yoff;      // From the top of the line (baseline = ascent)
    uint8_t adv;      // Pen advance
    uint32_t off;     // Into bits
} font_glyph_t;

typedef struct
{
    const font_glyph_t *glyphs;
    int count;
    const unsigned char *bits;
    uint8_t height;   // Line height (ascent + descent)
    uint8_t ascent;
} font_t;

extern const font_t font_small;   // DejaVu Sans 10 px
extern const font_t font_bold;    // DejaVu Sans Bold 10 px
extern const font_t font_title;   // DejaVu Sans Bold 13 px

#endif

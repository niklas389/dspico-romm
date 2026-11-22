// SPDX-License-Identifier: MIT
#ifndef ROMM_IMGDEC_H
#define ROMM_IMGDEC_H

// Image decoding straight into a small RGB15 picture (covers):
//   PNG  (not interlaced): own streaming decoder, zlib inflate row by row,
//        each row scaled down at once (no full-size buffer)
//   JPEG (baseline): TJpgDec, decoded at 1/2..1/8 size by the IDCT itself
//   anything else (interlaced PNG, progressive JPEG): stb_image, full size
// The PNG and JPEG paths yield to other threads every few milliseconds, so
// the UI keeps running while a cover decodes.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int w, h;                // Source size
    const char *how;         // "png", "jpeg/4", "stb", ...
    uint32_t inflate_ms;     // PNG: time in zlib inflate
    uint32_t rows_ms;        // PNG: unfiltering, conversion and scaling
} imgdec_info_t;

// fill: crop to exactly maxw x maxh; else the whole picture fitted inside.
bool imgdec_scaled(const uint8_t *data, size_t len, bool fill, int maxw, int maxh,
                   uint16_t *out, int *ow, int *oh, imgdec_info_t *info);

#endif

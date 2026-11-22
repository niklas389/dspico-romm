// SPDX-License-Identifier: MIT
// Cover image scaling (no SD or network use; also built by the host test).
#include "covers.h"

// Box filter: the source rectangle (sx, sy, sw, sh) into dw x dh RGB15
static void scale_box(const uint8_t *rgb, int w, int sx, int sy, int sw, int sh, uint16_t *out,
                      int dw, int dh)
{
    for (int y = 0; y < dh; y++)
    {
        int y0 = sy + y * sh / dh, y1 = sy + (y + 1) * sh / dh;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++)
        {
            int x0 = sx + x * sw / dw, x1 = sx + (x + 1) * sw / dw;
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
            {
                const uint8_t *p = rgb + ((size_t)yy * w + x0) * 3;
                for (int xx = x0; xx < x1; xx++, p += 3)
                {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    n++;
                }
            }
            r /= n;
            g /= n;
            b /= n;
            out[y * dw + x] = (uint16_t)(0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3));
        }
    }
}

void cover_convert(const uint8_t *rgb, int w, int h, uint16_t *top, uint16_t *tile)
{
    // Top: fill 3:4, cropping the longer side around the centre
    int sw = w, sh = h;
    if (w * COVER_H > h * COVER_W)
        sw = h * COVER_W / COVER_H;
    else
        sh = w * COVER_H / COVER_W;
    scale_box(rgb, w, (w - sw) / 2, (h - sh) / 2, sw, sh, top, COVER_W, COVER_H);

    // Tile: square; on portrait covers the upper part (title artwork)
    int s = w < h ? w : h;
    int ty = h > s ? (h - s) / 4 : 0;
    int tx = w > s ? (w - s) / 2 : 0;
    scale_box(rgb, w, tx, ty, s, s, tile, TILE_S, TILE_S);
}


// SPDX-License-Identifier: MIT
// Cover image scaling (no SD or network use; also built by the host test).
#include "covers.h"

#include <stdlib.h>
#include <string.h>

// Background for transparent pixels (UI_BG)
#define BG_R 236
#define BG_G 238
#define BG_B 241

void rs_geometry(int w, int h, bool fill, int maxw, int maxh, int *cx, int *cy, int *cw,
                 int *ch, int *dw, int *dh)
{
    if (fill)
    {
        // Fills maxw x maxh, cropping the longer side around the centre
        int sw = w, sh = h;
        if ((long)w * maxh > (long)h * maxw)
            sw = (int)((long)h * maxw / maxh);
        else
            sh = (int)((long)w * maxh / maxw);
        *cx = (w - sw) / 2;
        *cy = (h - sh) / 2;
        *cw = sw;
        *ch = sh;
        *dw = maxw;
        *dh = maxh;
        return;
    }
    // Whole image, aspect ratio kept
    int ow = maxw, oh = (int)((long)h * maxw / w);
    if (oh > maxh)
    {
        oh = maxh;
        ow = (int)((long)w * maxh / h);
    }
    *cx = *cy = 0;
    *cw = w;
    *ch = h;
    *dw = ow < 1 ? 1 : ow;
    *dh = oh < 1 ? 1 : oh;
}

bool rs_init(rowscale_t *rs, int w, int h, bool fill, int maxw, int maxh, uint16_t *out)
{
    memset(rs, 0, sizeof(*rs));
    rs->w = w;
    rs->h = h;
    rs->out = out;
    rs_geometry(w, h, fill, maxw, maxh, &rs->cx, &rs->cy, &rs->cw, &rs->ch, &rs->dw, &rs->dh);
    rs->acc = calloc(rs->dw * 4, sizeof(uint32_t));
    rs->xmap = malloc(rs->cw * sizeof(uint16_t));
    rs->xuse = calloc(rs->cw, 1);
    rs->yuse = calloc(rs->ch, 1);
    if (!rs->acc || !rs->xmap || !rs->xuse || !rs->yuse)
    {
        rs_free(rs);
        return false;
    }
    for (int x = 0; x < rs->cw; x++)
        rs->xmap[x] = (uint16_t)((long)x * rs->dw / rs->cw);
    // Up to 3 evenly spaced samples per output pixel and axis (a 10x
    // reduction averages 9 instead of 100 source pixels)
    for (int o = 0; o < rs->dw; o++)
    {
        int a = (int)((long)o * rs->cw / rs->dw), b = (int)((long)(o + 1) * rs->cw / rs->dw);
        if (b <= a)
            b = a + 1;
        int n = b - a < 3 ? b - a : 3;
        for (int k = 0; k < n; k++)
            rs->xuse[a + (b - a) * (2 * k + 1) / (2 * n)] = 1;
    }
    for (int o = 0; o < rs->dh; o++)
    {
        int a = (int)((long)o * rs->ch / rs->dh), b = (int)((long)(o + 1) * rs->ch / rs->dh);
        if (b <= a)
            b = a + 1;
        int n = b - a < 3 ? b - a : 3;
        for (int k = 0; k < n; k++)
            rs->yuse[a + (b - a) * (2 * k + 1) / (2 * n)] = 1;
    }
    rs->oy = -1;
    return true;
}

static void flush(rowscale_t *rs)
{
    if (rs->oy < 0 || rs->oy >= rs->dh)
        return;
    uint16_t *o = rs->out + rs->oy * rs->dw;
    for (int x = 0; x < rs->dw; x++)
    {
        uint32_t *a = &rs->acc[x * 4];
        uint32_t n = a[3] ? a[3] : 1;
        uint32_t r = a[0] / n, g = a[1] / n, b = a[2] / n;
        o[x] = (uint16_t)(0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3));
    }
    memset(rs->acc, 0, rs->dw * 4 * sizeof(uint32_t));
}

void rs_row(rowscale_t *rs, int y, const uint8_t *px, int bpp)
{
    if (!rs_row_wanted(rs, y))
        return;
    int oy = (int)((long)(y - rs->cy) * rs->dh / rs->ch);
    if (oy != rs->oy)
    {
        flush(rs);
        rs->oy = oy;
    }
    const uint8_t *p = px + rs->cx * bpp;
    for (int x = 0; x < rs->cw; x++, p += bpp)
    {
        if (!rs->xuse[x])
            continue;
        uint32_t *a = &rs->acc[rs->xmap[x] * 4];
        uint32_t r = p[0], g = p[1], b = p[2];
        if (bpp == 4 && p[3] != 255)
        {
            // Transparent parts over the UI background
            uint32_t al = p[3];
            r = (r * al + BG_R * (255 - al)) / 255;
            g = (g * al + BG_G * (255 - al)) / 255;
            b = (b * al + BG_B * (255 - al)) / 255;
        }
        a[0] += r;
        a[1] += g;
        a[2] += b;
        a[3]++;
    }
}

void rs_finish(rowscale_t *rs)
{
    flush(rs);
    rs->oy = -1;
}

void rs_free(rowscale_t *rs)
{
    free(rs->acc);
    free(rs->xmap);
    free(rs->xuse);
    free(rs->yuse);
    rs->acc = NULL;
    rs->xmap = NULL;
    rs->xuse = NULL;
    rs->yuse = NULL;
}

static void whole(const uint8_t *rgb, int w, int h, bool fill, int maxw, int maxh,
                  uint16_t *out, int *ow, int *oh)
{
    rowscale_t rs;
    if (!rs_init(&rs, w, h, fill, maxw, maxh, out))
    {
        *ow = *oh = 0;
        return;
    }
    for (int y = 0; y < h; y++)
        rs_row(&rs, y, rgb + (size_t)y * w * 3, 3);
    rs_finish(&rs);
    *ow = rs.dw;
    *oh = rs.dh;
    rs_free(&rs);
}

void cover_fit(const uint8_t *rgb, int w, int h, uint16_t *out, int maxw, int maxh, int *ow,
               int *oh)
{
    whole(rgb, w, h, false, maxw, maxh, out, ow, oh);
}

void cover_fill(const uint8_t *rgb, int w, int h, uint16_t *out, int dw, int dh)
{
    int ow, oh;
    whole(rgb, w, h, true, dw, dh, out, &ow, &oh);
}

// SPDX-License-Identifier: MIT
#include "imgdec.h"

#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "covers.h"
#include "tjpgd.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

// --- Yielding -------------------------------------------------------------------

#ifdef IMGDEC_HOST
static void maybe_yield(void)
{
}
static uint32_t ticks(void)
{
    return 0;
}
#define TICKS_PER_MS 1
#else
#include <nds.h>

// Gives the other threads (UI, network) a turn about every 4 ms
static void maybe_yield(void)
{
    static uint32_t last;
    uint32_t now = cpuGetTiming(); // Bus clock ticks (33.5 MHz)
    if (now - last > BUS_CLOCK / 250)
    {
        cothread_yield();
        last = cpuGetTiming();
    }
}

static uint32_t ticks(void)
{
    return cpuGetTiming();
}
#define TICKS_PER_MS (BUS_CLOCK / 1000)

// zlib state in ITCM when free (unzip.c), like for zip extraction
extern voidpf unzip_zalloc(voidpf opaque, uInt items, uInt size);
extern void unzip_zfree(voidpf opaque, voidpf p);
#endif

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// --- PNG (streaming) -------------------------------------------------------------

static inline int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

static void unfilter(uint8_t *cur, const uint8_t *prev, int n, int bpp, int type)
{
    switch (type)
    {
        case 1:
            for (int i = bpp; i < n; i++)
                cur[i] += cur[i - bpp];
            break;
        case 2:
            for (int i = 0; i < n; i++)
                cur[i] += prev[i];
            break;
        case 3:
            for (int i = 0; i < bpp; i++)
                cur[i] += prev[i] >> 1;
            for (int i = bpp; i < n; i++)
                cur[i] += (cur[i - bpp] + prev[i]) >> 1;
            break;
        case 4:
            for (int i = 0; i < bpp; i++)
                cur[i] += prev[i];
            for (int i = bpp; i < n; i++)
                cur[i] += (uint8_t)paeth(cur[i - bpp], prev[i], prev[i - bpp]);
            break;
        default:
            break;
    }
}

// 0: not a PNG we stream (try stb), 1: ok, -1: broken
static int png_stream(const uint8_t *d, size_t len, bool fill, int maxw, int maxh,
                      uint16_t *out, int *ow, int *oh, imgdec_info_t *info)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (len < 33 || memcmp(d, sig, 8) || memcmp(d + 12, "IHDR", 4))
        return 0;
    int w = (int)be32(d + 16), h = (int)be32(d + 20);
    int depth = d[24], ctype = d[25], interlace = d[28];
    if (interlace || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return 0;
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!ch || (depth != 8 && depth != 16 && !(depth < 8 && (ctype == 0 || ctype == 3))))
        return 0;
    info->w = w;
    info->h = h;
    info->how = "png";

    int bits = ch * depth;
    int bpp = (bits + 7) / 8;              // Filter distance in bytes
    int stride = (w * bits + 7) / 8;
    uint8_t pal[256 * 4];
    memset(pal, 255, sizeof(pal));
    for (int i = 0; i < 256; i++)
        pal[i * 4] = pal[i * 4 + 1] = pal[i * 4 + 2] = (uint8_t)i;

    rowscale_t rs;
    if (!rs_init(&rs, w, h, fill, maxw, maxh, out))
        return -1;
    uint8_t *rows = calloc(2, stride + 1);
    uint8_t *rgba = malloc((size_t)w * 4);
    z_stream z;
    memset(&z, 0, sizeof(z));
    int ret = -1;
#ifndef IMGDEC_HOST
    z.zalloc = unzip_zalloc;
    z.zfree = unzip_zfree;
#endif
    if (!rows || !rgba || inflateInit(&z) != Z_OK)
        goto done;
    uint32_t t_inf = 0, t_row = 0;

    uint8_t *prev = rows, *cur = rows + stride + 1;
    int y = 0, filled = 0;
    bool zend = false;
    size_t pos = 8;
    while (pos + 12 <= len && y < h)
    {
        uint32_t clen = be32(d + pos);
        const uint8_t *type = d + pos + 4, *cdata = d + pos + 8;
        if (clen > len - pos - 12)
            break;
        if (!memcmp(type, "PLTE", 4))
        {
            for (uint32_t i = 0; i < clen / 3 && i < 256; i++)
            {
                pal[i * 4] = cdata[i * 3];
                pal[i * 4 + 1] = cdata[i * 3 + 1];
                pal[i * 4 + 2] = cdata[i * 3 + 2];
            }
        }
        else if (!memcmp(type, "tRNS", 4) && ctype == 3)
        {
            for (uint32_t i = 0; i < clen && i < 256; i++)
                pal[i * 4 + 3] = cdata[i];
        }
        else if (!memcmp(type, "IDAT", 4) && !zend)
        {
            z.next_in = (Bytef *)cdata;
            z.avail_in = clen;
            while (z.avail_in && y < h)
            {
                z.next_out = cur + filled;
                z.avail_out = (uInt)(stride + 1 - filled);
                uint32_t t0 = ticks();
                int r = inflate(&z, Z_NO_FLUSH);
                t_inf += ticks() - t0;
                filled = stride + 1 - (int)z.avail_out;
                if (r == Z_STREAM_END)
                    zend = true;
                else if (r != Z_OK && r != Z_BUF_ERROR)
                    goto done;
                if (filled < stride + 1)
                {
                    if (zend)
                        break;
                    continue;
                }
                // One complete row: filter byte + data. Every row is
                // unfiltered (the next one depends on it); only the sampled
                // rows and columns are converted and scaled.
                uint32_t t1 = ticks();
                unfilter(cur + 1, prev + 1, stride, bpp, cur[0]);
                const uint8_t *s = cur + 1;
                bool want = rs_row_wanted(&rs, y);
                for (int x = rs.cx; want && x < rs.cx + rs.cw; x++)
                {
                    if (!rs.xuse[x - rs.cx])
                        continue;
                    uint8_t *o = rgba + x * 4;
                    if (depth < 8)
                    {
                        int v = (s[(x * depth) >> 3] >> (8 - depth - ((x * depth) & 7))) &
                                ((1 << depth) - 1);
                        if (ctype == 0)
                            v = v * 255 / ((1 << depth) - 1);
                        memcpy(o, &pal[v * 4], 4);
                        continue;
                    }
                    int k = depth == 16 ? 2 : 1; // 16 bit: high byte
                    const uint8_t *q = s + x * ch * k;
                    switch (ctype)
                    {
                        case 0: o[0] = o[1] = o[2] = q[0]; o[3] = 255; break;
                        case 2: o[0] = q[0]; o[1] = q[k]; o[2] = q[2 * k]; o[3] = 255; break;
                        case 3: memcpy(o, &pal[q[0] * 4], 4); break;
                        case 4: o[0] = o[1] = o[2] = q[0]; o[3] = q[k]; break;
                        default: o[0] = q[0]; o[1] = q[k]; o[2] = q[2 * k]; o[3] = q[3 * k]; break;
                    }
                }
                if (want)
                    rs_row(&rs, y, rgba, 4);
                y++;
                t_row += ticks() - t1;
                uint8_t *t = prev;
                prev = cur;
                cur = t;
                filled = 0;
                maybe_yield();
            }
        }
        else if (!memcmp(type, "IEND", 4))
            break;
        pos += 12 + clen;
    }
    info->inflate_ms = t_inf / TICKS_PER_MS;
    info->rows_ms = t_row / TICKS_PER_MS;
    if (y == h)
    {
        rs_finish(&rs);
        *ow = rs.dw;
        *oh = rs.dh;
        ret = 1;
    }

done:
    inflateEnd(&z);
    free(rows);
    free(rgba);
    rs_free(&rs);
    return ret;
}

// --- JPEG (TJpgDec, scaled IDCT) --------------------------------------------------

typedef struct
{
    const uint8_t *data;
    size_t len, pos;
    uint8_t *rgb;            // Scaled image, RGB888
    int w;                   // Its width
} jctx_t;

static size_t j_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jctx_t *c = jd->device;
    if (n > c->len - c->pos)
        n = c->len - c->pos;
    if (buf)
        memcpy(buf, c->data + c->pos, n);
    c->pos += n;
    return n;
}

static int j_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jctx_t *c = jd->device;
    const uint8_t *s = bitmap;
    int bw = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; y++, s += bw * 3)
        memcpy(c->rgb + ((size_t)y * c->w + r->left) * 3, s, (size_t)bw * 3);
    maybe_yield();
    return 1;
}

static int jpeg_scaled(const uint8_t *d, size_t len, bool fill, int maxw, int maxh,
                       uint16_t *out, int *ow, int *oh, imgdec_info_t *info)
{
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return 0;
    static uint8_t pool[16384];
    JDEC jd;
    jctx_t c = { d, len, 0, NULL, 0 };
    if (jd_prepare(&jd, j_in, pool, sizeof(pool), &c) != JDR_OK)
        return 0; // Progressive or unusual: stb
    info->w = jd.width;
    info->h = jd.height;

    // Largest IDCT scale that still leaves at least the output size
    int cx, cy, cw, chh, dw, dh;
    rs_geometry(jd.width, jd.height, fill, maxw, maxh, &cx, &cy, &cw, &chh, &dw, &dh);
    int scale = 0;
    while (scale < 3 && (cw >> (scale + 1)) >= dw && (chh >> (scale + 1)) >= dh)
        scale++;
    static const char *const how[4] = { "jpeg/1", "jpeg/2", "jpeg/4", "jpeg/8" };
    info->how = how[scale];
    // Written area: floor(size >> scale); the buffer has the rounded-up size
    int sw = jd.width >> scale, sh = jd.height >> scale;
    if (sw < 1 || sh < 1)
        return 0;
    c.w = (jd.width + (1 << scale) - 1) >> scale;
    c.rgb = calloc((size_t)c.w * (((jd.height + (1 << scale) - 1) >> scale) + 1), 3);
    if (!c.rgb)
        return -1;
    int ret = -1;
    if (jd_decomp(&jd, j_out, (uint8_t)scale) == JDR_OK)
    {
        rowscale_t rs;
        if (rs_init(&rs, sw, sh, fill, maxw, maxh, out))
        {
            for (int y = 0; y < sh; y++)
                rs_row(&rs, y, c.rgb + (size_t)y * c.w * 3, 3);
            rs_finish(&rs);
            *ow = rs.dw;
            *oh = rs.dh;
            rs_free(&rs);
            ret = 1;
        }
    }
    free(c.rgb);
    return ret;
}

// --- Fallback --------------------------------------------------------------------

static bool stb_scaled(const uint8_t *d, size_t len, bool fill, int maxw, int maxh,
                       uint16_t *out, int *ow, int *oh, imgdec_info_t *info)
{
    int w, h, comp;
    if (!stbi_info_from_memory(d, (int)len, &w, &h, &comp) || (long)w * h > 1600L * 1600L ||
        w < 8 || h < 8)
        return false;
    uint8_t *px = stbi_load_from_memory(d, (int)len, &w, &h, &comp, 4);
    if (!px)
        return false;
    info->w = w;
    info->h = h;
    info->how = "stb";
    rowscale_t rs;
    bool ok = rs_init(&rs, w, h, fill, maxw, maxh, out);
    if (ok)
    {
        for (int y = 0; y < h; y++)
            rs_row(&rs, y, px + (size_t)y * w * 4, 4);
        rs_finish(&rs);
        *ow = rs.dw;
        *oh = rs.dh;
        rs_free(&rs);
    }
    stbi_image_free(px);
    return ok;
}

bool imgdec_scaled(const uint8_t *data, size_t len, bool fill, int maxw, int maxh,
                   uint16_t *out, int *ow, int *oh, imgdec_info_t *info)
{
    imgdec_info_t dummy;
    if (!info)
        info = &dummy;
    memset(info, 0, sizeof(*info));
    int r = png_stream(data, len, fill, maxw, maxh, out, ow, oh, info);
    if (r == 0)
        r = jpeg_scaled(data, len, fill, maxw, maxh, out, ow, oh, info);
    if (r == 1)
        return true;
    return stb_scaled(data, len, fill, maxw, maxh, out, ow, oh, info);
}

// SPDX-License-Identifier: MIT
#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint16_t *gfx_top;
uint16_t *gfx_bot;

static bool dirty_top, dirty_bot;

bool gfx_alloc(void)
{
    if (!gfx_top)
        gfx_top = aligned_alloc(32, GFX_W * GFX_H * 2);
    if (!gfx_bot)
        gfx_bot = aligned_alloc(32, GFX_W * GFX_H * 2);
    return gfx_top && gfx_bot;
}

void gfx_dirty(uint16_t *fb)
{
    if (fb == gfx_top)
        dirty_top = true;
    else
        dirty_bot = true;
}

#ifndef GFX_HOST
#include <nds.h>

void gfx_init_bottom(void)
{
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    dirty_bot = true;
}

void gfx_init(void)
{
    gfx_alloc();
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    gfx_init_bottom();
    gfx_clear(gfx_top, GFX_RGB(236, 238, 241));
    gfx_clear(gfx_bot, GFX_RGB(236, 238, 241));
    dirty_top = dirty_bot = true;
}

static void copy_screen(volatile uint16_t *dst, const uint16_t *src)
{
    // 32-bit stores only (VRAM ignores byte writes)
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    for (int i = 0; i < GFX_W * GFX_H / 2; i += 8)
    {
        d[i] = s[i];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2];
        d[i + 3] = s[i + 3];
        d[i + 4] = s[i + 4];
        d[i + 5] = s[i + 5];
        d[i + 6] = s[i + 6];
        d[i + 7] = s[i + 7];
    }
}

void gfx_present_now(void)
{
    if (dirty_top)
        copy_screen(BG_GFX, gfx_top);
    if (dirty_bot)
        copy_screen(BG_GFX_SUB, gfx_bot);
    dirty_top = dirty_bot = false;
}

void gfx_present(void)
{
    cothread_yield_irq(IRQ_VBLANK); // Lets the network threads run too
    if (dirty_top)
        copy_screen(BG_GFX, gfx_top);
    if (dirty_bot)
        copy_screen(BG_GFX_SUB, gfx_bot);
    dirty_top = dirty_bot = false;
}
#endif

gfx_color_t gfx_mix(gfx_color_t a, gfx_color_t b, int alpha)
{
    if (alpha >= 256)
        return a | 0x8000;
    if (alpha <= 0)
        return b | 0x8000;
    int inv = 256 - alpha;
    int r = ((a & 31) * alpha + (b & 31) * inv) >> 8;
    int g = (((a >> 5) & 31) * alpha + ((b >> 5) & 31) * inv) >> 8;
    int bl = (((a >> 10) & 31) * alpha + ((b >> 10) & 31) * inv) >> 8;
    return (gfx_color_t)(0x8000 | bl << 10 | g << 5 | r);
}

static inline void put(uint16_t *fb, int x, int y, gfx_color_t c, int alpha)
{
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H || alpha <= 0)
        return;
    uint16_t *p = &fb[y * GFX_W + x];
    *p = alpha >= 256 ? (c | 0x8000) : gfx_mix(c, *p, alpha);
}

void gfx_fill(uint16_t *fb, int x, int y, int w, int h, gfx_color_t c)
{
    gfx_dirty(fb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0)
        return;
    c |= 0x8000;
    for (int j = 0; j < h; j++)
    {
        uint16_t *p = &fb[(y + j) * GFX_W + x];
        for (int i = 0; i < w; i++)
            p[i] = c;
    }
}

void gfx_clear(uint16_t *fb, gfx_color_t c)
{
    gfx_fill(fb, 0, 0, GFX_W, GFX_H, c);
}

void gfx_vgrad(uint16_t *fb, int x, int y, int w, int h, gfx_color_t c0, gfx_color_t c1)
{
    for (int j = 0; j < h; j++)
        gfx_fill(fb, x, y + j, w, 1, gfx_mix(c1, c0, h > 1 ? j * 256 / (h - 1) : 0));
}

void gfx_shade(uint16_t *fb, int x, int y, int w, int h, gfx_color_t c, int alpha)
{
    gfx_dirty(fb);
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            put(fb, i, j, c, alpha);
}

// --- Rounded corners: coverage of corner pixels, per radius --------------

#define R_MAX 24
static uint8_t cov_tab[R_MAX + 1][R_MAX][R_MAX]; // [r][dy][dx], 0..255 (+1 = 256)
static bool cov_done[R_MAX + 1];

// Coverage of pixel (dx, dy) of the top-left corner square of radius r
// (0..256); the circle centre is at (r, r).
static int coverage(int r, int dx, int dy)
{
    if (r <= 0)
        return 256;
    if (r > R_MAX)
        r = R_MAX;
    if (!cov_done[r])
    {
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++)
            {
                // 4x4 supersampling
                int in = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                    {
                        float px = i + (sx + 0.5f) / 4 - r;
                        float py = j + (sy + 0.5f) / 4 - r;
                        in += px * px + py * py <= (float)r * r;
                    }
                cov_tab[r][j][i] = (uint8_t)(in * 255 / 16);
            }
        cov_done[r] = true;
    }
    if (dx >= r || dy >= r)
        return 256;
    int v = cov_tab[r][dy][dx];
    return v == 255 ? 256 : v;
}

// Coverage of pixel (i, j) inside a w x h box with radius r
static int box_cov(int i, int j, int w, int h, int r)
{
    int dx = i < r ? i : (i >= w - r ? w - 1 - i : r);
    int dy = j < r ? j : (j >= h - r ? h - 1 - j : r);
    if (dx >= r || dy >= r)
        return 256;
    return coverage(r, dx, dy);
}

static void rrect_fill(uint16_t *fb, int x, int y, int w, int h, int r, gfx_color_t c)
{
    if (w <= 0 || h <= 0)
        return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    // Middle band without corners
    gfx_fill(fb, x, y + r, w, h - 2 * r, c);
    for (int j = 0; j < r; j++)
    {
        gfx_fill(fb, x + r, y + j, w - 2 * r, 1, c);
        gfx_fill(fb, x + r, y + h - 1 - j, w - 2 * r, 1, c);
        for (int i = 0; i < r; i++)
        {
            int a = coverage(r, i, j);
            put(fb, x + i, y + j, c, a);
            put(fb, x + w - 1 - i, y + j, c, a);
            put(fb, x + i, y + h - 1 - j, c, a);
            put(fb, x + w - 1 - i, y + h - 1 - j, c, a);
        }
    }
}

void gfx_rrect(uint16_t *fb, int x, int y, int w, int h, int r, gfx_color_t fill,
               gfx_color_t border, int border_w)
{
    gfx_dirty(fb);
    if (border_w > 0)
    {
        rrect_fill(fb, x, y, w, h, r, border);
        rrect_fill(fb, x + border_w, y + border_w, w - 2 * border_w, h - 2 * border_w,
                   r > border_w ? r - border_w : 0, fill);
    }
    else
        rrect_fill(fb, x, y, w, h, r, fill);
}

void gfx_rframe(uint16_t *fb, int x, int y, int w, int h, int r, gfx_color_t c, int t)
{
    gfx_dirty(fb);
    // Outer shape minus inner shape, per pixel
    int ri = r > t ? r - t : 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            int outer = box_cov(i, j, w, h, r);
            int inner = 0;
            if (i >= t && j >= t && i < w - t && j < h - t)
                inner = box_cov(i - t, j - t, w - 2 * t, h - 2 * t, ri);
            int a = outer - inner;
            if (a > 0)
                put(fb, x + i, y + j, c, a);
        }
}

void gfx_circle(uint16_t *fb, int cx, int cy, int r, gfx_color_t c)
{
    gfx_rrect(fb, cx - r, cy - r, 2 * r, 2 * r, r, c, 0, 0);
}

void gfx_blit(uint16_t *fb, int x, int y, int dw, int dh, const uint16_t *img, int w,
              int h, int radius)
{
    gfx_dirty(fb);
    if (dw <= 0 || dh <= 0)
        return;
    for (int j = 0; j < dh; j++)
    {
        int yy = y + j;
        if ((unsigned)yy >= GFX_H)
            continue;
        const uint16_t *row = img + (j * h / dh) * w;
        for (int i = 0; i < dw; i++)
        {
            int xx = x + i;
            if ((unsigned)xx >= GFX_W)
                continue;
            int a = radius ? box_cov(i, j, dw, dh, radius) : 256;
            put(fb, xx, yy, row[i * w / dw], a);
        }
    }
}

// --- Text -------------------------------------------------------------------

// Decodes one UTF-8 sequence; returns the code point and advances *s
static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int extra = 0;
    if (c >= 0xF0) { c &= 0x07; extra = 3; }
    else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
    else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
    else if (c >= 0x80) { c = 0xFFFD; }
    while (extra-- > 0)
    {
        if ((*p & 0xC0) != 0x80)
        {
            c = 0xFFFD;
            break;
        }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = (const char *)p;
    return c;
}

static const font_glyph_t *glyph(const font_t *f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp)
            return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (cp == '\t')
        return glyph(f, ' ');
    return cp == 0xFFFD ? &f->glyphs[0] : glyph(f, 0xFFFD);
}

static void draw_glyph(uint16_t *fb, const font_t *f, const font_glyph_t *g, int x, int y,
                       gfx_color_t c)
{
    const unsigned char *bits = f->bits + g->off;
    int stride = (g->w + 1) / 2;
    for (int j = 0; j < g->h; j++)
    {
        int yy = y + g->yoff + j;
        if ((unsigned)yy >= GFX_H)
            continue;
        for (int i = 0; i < g->w; i++)
        {
            int v = (bits[j * stride + i / 2] >> ((i & 1) * 4)) & 15;
            if (v)
                put(fb, x + g->xoff + i, yy, c, v == 15 ? 256 : v * 17);
        }
    }
}

static int width_n(const font_t *f, const char *s, const char *end)
{
    int w = 0;
    while (*s && (!end || s < end))
        w += glyph(f, utf8_next(&s))->adv;
    return w;
}

int gfx_text_width(const font_t *f, const char *s)
{
    return width_n(f, s, NULL);
}

static int draw_run(uint16_t *fb, const font_t *f, int x, int y, gfx_color_t c,
                    const char *s, const char *end)
{
    int x0 = x;
    while (*s && (!end || s < end))
    {
        const font_glyph_t *g = glyph(f, utf8_next(&s));
        draw_glyph(fb, f, g, x, y, c);
        x += g->adv;
    }
    return x - x0;
}

int gfx_text(uint16_t *fb, const font_t *f, int x, int y, gfx_color_t c, const char *s,
             int align, int maxw)
{
    if (!s)
        return 0;
    gfx_dirty(fb);
    int w = gfx_text_width(f, s);
    const char *end = NULL;
    bool ellipsis = false;
    if (maxw > 0 && w > maxw)
    {
        int ew = glyph(f, 0x2026)->adv;
        const char *p = s;
        int acc = 0;
        while (*p)
        {
            const char *q = p;
            int a = glyph(f, utf8_next(&q))->adv;
            if (acc + a + ew > maxw)
                break;
            acc += a;
            p = q;
        }
        end = p;
        w = acc + ew;
        ellipsis = true;
    }
    if (align == GFX_CENTER)
        x -= w / 2;
    else if (align == GFX_RIGHT)
        x -= w;
    int dx = draw_run(fb, f, x, y, c, s, end);
    if (ellipsis)
        draw_glyph(fb, f, glyph(f, 0x2026), x + dx, y, c);
    return w;
}

// One wrapped line starting at s: *end = end of the text to draw, returns the
// start of the next line
static const char *wrap_line(const font_t *f, const char *s, int w, const char **end)
{
    const char *p = s, *brk = NULL;
    int acc = 0;
    while (*p && *p != '\n')
    {
        const char *q = p;
        int a = glyph(f, utf8_next(&q))->adv;
        if (acc + a > w)
            break;
        acc += a;
        p = q;
        if (*p == ' ' || *p == '\n' || !*p)
            brk = p;
    }
    if (*p && *p != '\n' && brk)
        p = brk; // Break at the last space
    if (p == s && *p && *p != '\n')
        utf8_next(&p); // At least one character per line
    *end = p;
    while (*p == ' ')
        p++;
    if (*p == '\n')
        p++;
    return p;
}

int gfx_text_wrap(uint16_t *fb, const font_t *f, int x, int y, int w, int max_lines,
                  gfx_color_t c, const char *s)
{
    int lines = 0;
    while (*s && lines < max_lines)
    {
        const char *end;
        const char *next = wrap_line(f, s, w, &end);
        if (lines == max_lines - 1 && *next)
        {
            // Last line: the rest, truncated
            char tmp[256];
            int n = 0;
            const char *q = s;
            while (*q && *q != '\n' && n < (int)sizeof(tmp) - 1)
                tmp[n++] = *q++;
            tmp[n] = 0;
            gfx_text(fb, f, x, y, c, tmp, GFX_LEFT, w);
        }
        else
        {
            gfx_dirty(fb);
            draw_run(fb, f, x, y, c, s, end);
        }
        lines++;
        y += f->height;
        s = next;
    }
    return lines;
}

int gfx_text_wrap_scroll(uint16_t *fb, const font_t *f, int x, int y, int w, int skip,
                         int max_lines, gfx_color_t c, const char *s)
{
    int lines = 0;
    gfx_dirty(fb);
    while (*s)
    {
        const char *end;
        const char *next = wrap_line(f, s, w, &end);
        if (lines >= skip && lines < skip + max_lines)
            draw_run(fb, f, x, y + (lines - skip) * f->height, c, s, end);
        lines++;
        s = next;
    }
    return lines;
}

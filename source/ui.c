// SPDX-License-Identifier: MIT
#include "ui.h"

#include <stdio.h>
#include <string.h>

// --- Helpers --------------------------------------------------------------------

gfx_color_t ui_name_color(const char *name)
{
    static const gfx_color_t pal[] = {
        GFX_RGB(208, 72, 64),  GFX_RGB(64, 96, 168),  GFX_RGB(220, 140, 48),
        GFX_RGB(44, 150, 168), GFX_RGB(104, 160, 64), GFX_RGB(120, 84, 168),
        GFX_RGB(200, 88, 140), GFX_RGB(72, 128, 112), GFX_RGB(176, 120, 72),
        GFX_RGB(84, 112, 200),
    };
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++)
        h = (h ^ *p) * 16777619u;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

void ui_initials(const char *name, char *out, int len)
{
    int n = 0;
    bool start = true;
    for (const char *p = name ? name : ""; *p && n < len - 1 && n < 3; p++)
    {
        unsigned char c = (unsigned char)*p;
        bool alnum = (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
        if (start && alnum)
            out[n++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        start = c == ' ' || c == '-' || c == ':' || c == '_';
    }
    out[n] = 0;
}

int ui_letter_of(const char *name)
{
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    // Skip leading punctuation such as quotes or brackets
    while (*p && !((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z')) &&
           *p < 0x80)
        p++;
    unsigned c = *p;
    if (c == 0xC3 && p[1]) // Latin-1 letters with accents
    {
        unsigned d = p[1] | 0x20; // Lower case
        if (d >= 0xA0 && d <= 0xA5) c = 'a';
        else if (d == 0xA7) c = 'c';
        else if (d >= 0xA8 && d <= 0xAB) c = 'e';
        else if (d >= 0xAC && d <= 0xAF) c = 'i';
        else if (d == 0xB1) c = 'n';
        else if ((d >= 0xB2 && d <= 0xB6) || d == 0xB8) c = 'o';
        else if (d >= 0xB9 && d <= 0xBC) c = 'u';
    }
    c |= 32;
    if (c >= 'a' && c <= 'z')
        return (int)(c - 'a') + 1;
    return 0;
}

static void hint_line(uint16_t *fb, const char *hint)
{
    if (hint)
        gfx_text(fb, &font_small, GFX_W / 2, GFX_H - 14, UI_MUTED, hint, GFX_CENTER,
                 GFX_W - 8);
}

static void background(uint16_t *fb)
{
    gfx_clear(fb, UI_BG);
}

static gfx_color_t kind_color(int kind)
{
    return kind == UI_OK ? UI_GREEN : kind == UI_WARN ? UI_ORANGE
         : kind == UI_ERROR ? UI_RED : UI_ACCENT;
}

static gfx_color_t kind_bg(int kind)
{
    return kind == UI_OK ? UI_GREEN_BG : kind == UI_WARN ? UI_ORANGE_BG
         : kind == UI_ERROR ? UI_RED_BG : GFX_RGB(226, 238, 252);
}

// Placeholder or image tile
static void tile(uint16_t *fb, int x, int y, int w, int h, const char *label,
                 gfx_color_t color, const uint16_t *img, int iw, int ih, int r)
{
    if (img)
    {
        // Whole picture, aspect ratio kept, centred (iw, ih: its size)
        int dw = w, dh = ih * w / iw;
        if (dh > h)
        {
            dh = h;
            dw = iw * h / ih;
        }
        int rr = r * dw / (w > 0 ? w : 1);
        gfx_blit(fb, x + (w - dw) / 2, y + (h - dh), dw, dh, img, iw, ih, rr > 2 ? rr : 2);
        return;
    }
    gfx_rrect(fb, x, y, w, h, r, color, 0, 0);
    const font_t *f = w >= 56 ? &font_title : &font_bold;
    gfx_text(fb, f, x + w / 2, y + (h - f->height) / 2, UI_WHITE, label, GFX_CENTER, w - 4);
}

static void badge(uint16_t *fb, int x, int y, int w, int kind, const char *text)
{
    gfx_rrect(fb, x, y, w, 17, 6, kind_bg(kind), kind_color(kind), 1);
    gfx_circle(fb, x + 9, y + 8, 3, kind_color(kind));
    gfx_text(fb, &font_small, x + 17, y + 2, kind_color(kind), text, GFX_LEFT, w - 21);
}

static void format_size(uint64_t bytes, char *out, size_t len)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, len, "%lu.%lu MB", (unsigned long)(bytes >> 20),
                 (unsigned long)(((bytes & 0xFFFFF) * 10) >> 20));
    else
        snprintf(out, len, "%lu KB", (unsigned long)((bytes + 1023) >> 10));
}

// --- Top screen -------------------------------------------------------------------

int ui_wifi_level = -2;
int ui_battery = -1;

// Battery: 4 segments; charging = green fill; empty = red
static void battery_icon(uint16_t *fb, int x, int y, int v)
{
    int level = v & 0xF;
    bool charging = v & 0x80;
    gfx_color_t frame_c = UI_WHITE;
    gfx_fill(fb, x, y, 18, 1, frame_c);
    gfx_fill(fb, x, y + 9, 18, 1, frame_c);
    gfx_fill(fb, x, y, 1, 10, frame_c);
    gfx_fill(fb, x + 17, y, 1, 10, frame_c);
    gfx_fill(fb, x + 18, y + 3, 2, 4, frame_c);
    gfx_color_t c = charging ? GFX_RGB(120, 230, 120) : level <= 1 ? UI_RED : UI_WHITE;
    int segs = level >= 15 ? 4 : level >= 11 ? 3 : level >= 7 ? 2 : level >= 3 ? 1 : 0;
    if (segs == 0 && !charging)
        gfx_fill(fb, x + 2, y + 2, 2, 6, c); // Almost empty: a red sliver
    for (int i = 0; i < segs; i++)
        gfx_fill(fb, x + 2 + i * 4, y + 2, 3, 6, c);
}

// Three signal bars; level -1 = not connected (crossed out)
static void wifi_icon(uint16_t *fb, int x, int y, int level)
{
    for (int i = 0; i < 3; i++)
    {
        int h = 4 + 3 * i;
        gfx_color_t c = level > i ? UI_WHITE : gfx_mix(UI_WHITE, UI_ACCENT, 90);
        gfx_fill(fb, x + i * 4, y + 11 - h, 3, h, c);
    }
    if (level < 0)
    {
        for (int i = 0; i < 9; i++) // Diagonal stroke
            gfx_fill(fb, x + 1 + i, y + 2 + i, 2, 1, UI_RED);
    }
}

void ui_top_header(uint16_t *fb, const char *left, const char *right)
{
    gfx_vgrad(fb, 0, 0, GFX_W, 16, UI_ACCENT_HI, UI_ACCENT);
    gfx_text(fb, &font_bold, 5, 1, UI_WHITE, left, GFX_LEFT, 160);
    int x = GFX_W - 5;
    if (right)
        x -= gfx_text(fb, &font_bold, x, 1, UI_WHITE, right, GFX_RIGHT, 90) + 6;
    if (ui_battery >= 0)
    {
        battery_icon(fb, x - 20, 3, ui_battery);
        x -= 27;
    }
    if (ui_wifi_level > -2)
        wifi_icon(fb, x - 11, 2, ui_wifi_level);
}

void ui_draw_game_top(uint16_t *fb, const ui_game_t *g)
{
    background(fb);
    ui_top_header(fb, "RomM", g->clock);

    // Cover: up to 90x120, whole picture centred in that area
    int cx = 8, cy = 24, cw = 90, ch = 120;
    char ini[8];
    ui_initials(g->name, ini, sizeof(ini));
    if (g->cover)
    {
        int x0 = cx + (cw - g->cover_w) / 2, y0 = cy + (ch - g->cover_h) / 2;
        gfx_rrect(fb, x0 - 1, y0 - 1, g->cover_w + 2, g->cover_h + 2, 5, UI_LINE, 0, 0);
        gfx_blit(fb, x0, y0, g->cover_w, g->cover_h, g->cover, g->cover_w, g->cover_h, 4);
    }
    else
    {
        gfx_rrect(fb, cx - 1, cy - 1, cw + 2, ch + 2, 7, UI_LINE, 0, 0);
        tile(fb, cx, cy, cw, ch, ini, ui_name_color(g->name), NULL, 0, 0, 6);
    }

    int x = 106, w = GFX_W - x - 6, y = 22;
    y += gfx_text_wrap(fb, &font_title, x, y, w, 2, UI_TEXT, g->name) * font_title.height;
    y += 3;
    char line[96];
    if (g->year)
        snprintf(line, sizeof(line), "%s  \xC2\xB7  %d", g->platform, g->year);
    else
        snprintf(line, sizeof(line), "%s", g->platform);
    gfx_text(fb, &font_small, x, y, UI_MUTED, line, GFX_LEFT, w);
    y += font_small.height;
    line[0] = 0;
    if (g->regions && g->regions[0])
        snprintf(line, sizeof(line), "%s", g->regions);
    if (g->players && g->players[0])
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s player%s",
                 line[0] ? "  \xC2\xB7  " : "", g->players,
                 strcmp(g->players, "1") ? "s" : "");
    if (line[0])
    {
        gfx_text(fb, &font_small, x, y, UI_MUTED, line, GFX_LEFT, w);
        y += font_small.height;
    }
    if (g->genres && g->genres[0])
    {
        gfx_text(fb, &font_small, x, y, UI_MUTED, g->genres, GFX_LEFT, w);
        y += font_small.height;
    }

    // Badges: SD card, save
    char sz[24], b[64];
    format_size(g->size, sz, sizeof(sz));
    int by = 108;
    if (g->on_sd)
        snprintf(b, sizeof(b), "On SD card  (%s)", sz);
    else
        snprintf(b, sizeof(b), "Not on SD  (%s%s)", sz, g->archive ? ", zip" : "");
    badge(fb, x, by, w, g->on_sd ? UI_OK : UI_INFO, b);
    if (g->save)
    {
        snprintf(b, sizeof(b), "Save: %s", g->save);
        badge(fb, x, by + 21, w, g->save_kind, b);
    }

    if (g->summary && g->summary[0])
        gfx_text_wrap(fb, &font_small, 8, 150, GFX_W - 16, 3, UI_TEXT, g->summary);
}

void ui_draw_platform_top(uint16_t *fb, const ui_platform_t *p)
{
    background(fb);
    ui_top_header(fb, "RomM", p->clock);

    gfx_rrect(fb, 15, 31, 82, 82, 13, UI_LINE, 0, 0);
    tile(fb, 16, 32, 80, 80, p->label, ui_name_color(p->name), NULL, 0, 0, 12);

    int x = 108, w = GFX_W - x - 6, y = 30;
    y += gfx_text_wrap(fb, &font_title, x, y, w, 2, UI_TEXT, p->name) * font_title.height;
    y += 4;
    char line[64];
    snprintf(line, sizeof(line), "%d game%s in RomM", p->games, p->games == 1 ? "" : "s");
    gfx_text(fb, &font_small, x, y, UI_MUTED, line, GFX_LEFT, w);
    y += font_small.height;
    if (p->on_sd >= 0)
    {
        snprintf(line, sizeof(line), "%d on the SD card", p->on_sd);
        gfx_text(fb, &font_small, x, y, UI_MUTED, line, GFX_LEFT, w);
        y += font_small.height;
    }
    snprintf(line, sizeof(line), "Folder: roms/%s/", p->folder);
    gfx_text(fb, &font_small, x, y, UI_MUTED, line, GFX_LEFT, w);
    y += font_small.height;
    if (p->hidden)
        badge(fb, x, y + 6, w, UI_WARN, "Hidden");

    if (p->note)
        gfx_text_wrap(fb, &font_small, 16, 134, GFX_W - 32, 3, UI_MUTED, p->note);
}

void ui_draw_splash_top(uint16_t *fb, const char *version, const char *line)
{
    gfx_vgrad(fb, 0, 0, GFX_W, GFX_H, UI_ACCENT_HI, UI_ACCENT);
    gfx_rrect(fb, 88, 40, 80, 80, 18, UI_WHITE, 0, 0);
    gfx_text(fb, &font_title, 128, 72, UI_ACCENT, "RomM", GFX_CENTER, 0);
    gfx_text(fb, &font_title, 128, 132, UI_WHITE, "RomM for DSi", GFX_CENTER, 0);
    if (version)
        gfx_text(fb, &font_small, 128, 150, UI_WHITE, version, GFX_CENTER, 0);
    if (line)
        gfx_text(fb, &font_small, 128, 172, UI_WHITE, line, GFX_CENTER, GFX_W - 16);
}

// --- Bottom screen: icon strip ------------------------------------------------

#define STRIP_Y    46  // Top of the selected tile
#define BIG        64
#define SMALL      44
#define SPACING    54  // Centre distance between small tiles

static void strip_tile(uint16_t *fb, const ui_tile_t *t, int off_q8)
{
    int a = off_q8 < 0 ? -off_q8 : off_q8; // |offset| in 1/256
    int grow = a >= 256 ? 0 : 256 - a;
    int size = SMALL + (BIG - SMALL) * grow / 256;
    int side = off_q8 < 0 ? -1 : 1;
    int push = a >= 256 ? 14 : 14 * a / 256; // Extra gap next to the big tile
    int cx = GFX_W / 2 + off_q8 * SPACING / 256 + side * push;
    int x = cx - size / 2;
    int y = STRIP_Y + (BIG - size) / 2;
    if (x > GFX_W || x + size < 0)
        return;
    int r = size / 6;
    tile(fb, x, y, size, size, t->label, t->color, t->img, t->img_w, t->img_h, r);
    if (t->flags & UI_TILE_DIMMED)
        gfx_shade(fb, x, y, size, size, UI_BG, 150);
    if (t->flags & UI_TILE_ON_SD)
    {
        int bx = x + size - 7, by = y + 7;
        gfx_circle(fb, bx, by, 7, UI_WHITE);
        gfx_circle(fb, bx, by, 6, UI_GREEN);
        gfx_text(fb, &font_bold, bx, by - 6, UI_WHITE, "\xE2\x9C\x93", GFX_CENTER, 0);
    }
    if (t->flags & UI_TILE_WARN)
    {
        int bx = x + 7, by = y + 7;
        gfx_circle(fb, bx, by, 7, UI_WHITE);
        gfx_circle(fb, bx, by, 6, UI_ORANGE);
        gfx_text(fb, &font_bold, bx, by - 6, UI_WHITE, "!", GFX_CENTER, 0);
    }
}

void ui_draw_strip(uint16_t *fb, const ui_strip_t *s)
{
    background(fb);

    // Title panel
    gfx_rrect(fb, 4, 4, GFX_W - 8, 20, 7, UI_PANEL, UI_LINE, 1);
    gfx_text(fb, &font_bold, 11, 7, UI_TEXT, s->title, GFX_LEFT, 150);
    int sx = GFX_W - 11;
    if (s->subtitle)
        sx -= gfx_text(fb, &font_small, sx, 7, UI_MUTED, s->subtitle, GFX_RIGHT, 90) + 7;
    if (s->status == 1)
    {
        // Updating: three pulsing dots
        for (int i = 0; i < 3; i++)
        {
            bool on = (s->anim / 8) % 3 == i;
            gfx_circle(fb, sx - 16 + i * 6, 14, 2, on ? UI_ACCENT : UI_FAINT);
        }
    }
    else if (s->status == 2)
    {
        // Offline: orange "!"
        gfx_circle(fb, sx - 5, 14, 6, UI_ORANGE);
        gfx_text(fb, &font_bold, sx - 5, 8, UI_WHITE, "!", GFX_CENTER, 0);
    }

    if (s->count == 0)
    {
        gfx_text(fb, &font_bold, GFX_W / 2, 80, UI_MUTED, "Nothing here", GFX_CENTER, 0);
        hint_line(fb, s->hint);
        return;
    }

    // Tiles: far ones first, the centre one last (on top)
    int centre = (s->pos_q8 + 128) >> 8;
    for (int d = 4; d >= 0; d--)
        for (int side = -1; side <= 1; side += 2)
        {
            if (d == 0 && side == 1)
                continue;
            int i = centre + side * d;
            if (i < 0 || i >= s->count)
                continue;
            strip_tile(fb, &s->tiles[i], i * 256 - s->pos_q8);
        }

    // Selection frame (fixed in the middle)
    gfx_rframe(fb, GFX_W / 2 - BIG / 2 - 4, STRIP_Y - 4, BIG + 8, BIG + 8, 14, UI_ACCENT, 2);

    const ui_tile_t *t = &s->tiles[s->sel];
    int y = STRIP_Y + BIG + 8;
    gfx_text(fb, &font_bold, GFX_W / 2, y, UI_TEXT, t->name, GFX_CENTER, GFX_W - 16);
    if (s->caption2)
        gfx_text(fb, &font_small, GFX_W / 2, y + 13, UI_MUTED, s->caption2, GFX_CENTER,
                 GFX_W - 16);

    if (s->letters)
    {
        // Letter bar
        int ly = 146;
        int step_q8 = (GFX_W - 16) * 256 / 27;
        if (s->letter >= 0 && s->letter < 27)
        {
            int x = 8 + (s->letter * step_q8 + step_q8 / 2) / 256;
            gfx_rrect(fb, x - 5, ly - 1, 11, 15, 4, UI_ACCENT, 0, 0);
        }
        // Letters after the highlight, the neighbours' letters stay readable
        for (int i = 0; i < 27; i++)
        {
            char ch[2] = { i == 0 ? '#' : (char)('A' + i - 1), 0 };
            int x = 8 + (i * step_q8 + step_q8 / 2) / 256;
            gfx_color_t c = i == s->letter ? UI_WHITE : s->letters[i] ? UI_TEXT : UI_FAINT;
            gfx_text(fb, &font_small, x, ly, c, ch, GFX_CENTER, 0);
        }
    }

    // Position bar
    int py = 165, pw = GFX_W - 16;
    gfx_rrect(fb, 8, py, pw, 6, 3, UI_BG2, 0, 0);
    int kw = pw / (s->count < 8 ? s->count : 8);
    if (kw < 10)
        kw = 10;
    int kx = s->count > 1 ? 8 + (pw - kw) * s->sel / (s->count - 1) : 8;
    gfx_rrect(fb, kx, py, kw, 6, 3, UI_ACCENT_HI, 0, 0);

    hint_line(fb, s->hint);
}

// --- Bottom screen: other pages ---------------------------------------------------

static void panel(uint16_t *fb, int x, int y, int w, int h)
{
    gfx_rrect(fb, x, y, w, h, 9, UI_PANEL, UI_LINE, 1);
}

void ui_draw_steps(uint16_t *fb, const char *title, const ui_step_t *steps, int n,
                   const char *hint)
{
    background(fb);
    panel(fb, 8, 8, GFX_W - 16, GFX_H - 34);
    gfx_text(fb, &font_title, 20, 16, UI_TEXT, title, GFX_LEFT, GFX_W - 40);
    int y = 40;
    for (int i = 0; i < n; i++)
    {
        const ui_step_t *s = &steps[i];
        gfx_color_t c = s->state == 2 ? UI_GREEN : s->state == 3 ? UI_RED
                      : s->state == 1 ? UI_ACCENT : UI_FAINT;
        gfx_circle(fb, 26, y + 6, 6, c);
        const char *sym = s->state == 2 ? "\xE2\x9C\x93" : s->state == 3 ? "\xE2\x9C\x97"
                        : s->state == 1 ? "\xE2\x80\xA2" : "";
        gfx_text(fb, &font_bold, 26, y, UI_WHITE, sym, GFX_CENTER, 0);
        gfx_text(fb, s->state == 1 ? &font_bold : &font_small, 40, y, s->state ? UI_TEXT : UI_MUTED,
                 s->text, GFX_LEFT, 110);
        if (s->detail)
            gfx_text(fb, &font_small, GFX_W - 20, y, UI_MUTED, s->detail, GFX_RIGHT, 92);
        y += 18;
    }
    hint_line(fb, hint);
}

void ui_draw_message(uint16_t *fb, int kind, const char *title, const char *body,
                     const char *hint)
{
    background(fb);
    panel(fb, 8, 8, GFX_W - 16, GFX_H - 34);
    gfx_circle(fb, 30, 30, 11, kind_color(kind));
    gfx_text(fb, &font_title, 30, 22, UI_WHITE,
             kind == UI_OK ? "\xE2\x9C\x93" : kind == UI_INFO ? "i" : "!", GFX_CENTER, 0);
    gfx_text_wrap(fb, &font_title, 48, 16, GFX_W - 68, 2, UI_TEXT, title);
    if (body)
        gfx_text_wrap(fb, &font_small, 20, 52, GFX_W - 40, 8, UI_TEXT, body);
    hint_line(fb, hint);
}

void ui_draw_progress(uint16_t *fb, const ui_progress_t *p)
{
    background(fb);
    panel(fb, 8, 8, GFX_W - 16, GFX_H - 34);
    gfx_text(fb, &font_title, 20, 16, UI_TEXT, p->title, GFX_LEFT, GFX_W - 40);
    gfx_text(fb, &font_small, 20, 36, UI_MUTED, p->name, GFX_LEFT, GFX_W - 40);

    // Phase chips
    int x = 20;
    for (int i = 0; i < 3 && p->phases[i]; i++)
    {
        int w = gfx_text_width(&font_small, p->phases[i]) + 16;
        bool cur = i == p->phase, done = i < p->phase;
        gfx_rrect(fb, x, 56, w, 16, 8, cur ? UI_ACCENT : done ? UI_GREEN_BG : UI_BG2, 0, 0);
        gfx_text(fb, &font_small, x + w / 2, 58, cur ? UI_WHITE : done ? UI_GREEN : UI_MUTED,
                 p->phases[i], GFX_CENTER, 0);
        x += w + 6;
    }

    // Bar
    int bx = 20, by = 86, bw = GFX_W - 40, bh = 14;
    gfx_rrect(fb, bx, by, bw, bh, 7, UI_BG2, 0, 0);
    if (p->permille >= 0)
    {
        int fw = bw * (p->permille > 1000 ? 1000 : p->permille) / 1000;
        if (fw > 0)
            gfx_rrect(fb, bx, by, fw < bh ? bh : fw, bh, 7, kind_color(p->kind), 0, 0);
        char pc[16];
        snprintf(pc, sizeof(pc), "%d%%", p->permille / 10);
        gfx_text(fb, &font_bold, GFX_W - 20, 106, UI_TEXT, pc, GFX_RIGHT, 0);
    }
    if (p->line1)
        gfx_text(fb, &font_small, 20, 106, UI_TEXT, p->line1, GFX_LEFT, 170);
    if (p->line2)
        gfx_text(fb, &font_small, 20, 122, UI_MUTED, p->line2, GFX_LEFT, GFX_W - 40);
    hint_line(fb, p->hint);
}

static void save_card(uint16_t *fb, int x, int y, int w, const char *who, const char *date,
                      const char *size)
{
    gfx_rrect(fb, x, y, w, 58, 8, date ? UI_PANEL : UI_BG, UI_LINE, 1);
    gfx_text(fb, &font_bold, x + w / 2, y + 5, UI_TEXT, who, GFX_CENTER, w - 8);
    if (date)
    {
        gfx_text(fb, &font_small, x + w / 2, y + 22, UI_MUTED, date, GFX_CENTER, w - 8);
        gfx_text(fb, &font_small, x + w / 2, y + 37, UI_MUTED, size, GFX_CENTER, w - 8);
    }
    else
        gfx_text(fb, &font_small, x + w / 2, y + 28, UI_FAINT, "no save", GFX_CENTER, w - 8);
}

void ui_draw_savesync(uint16_t *fb, const ui_savesync_t *s)
{
    background(fb);
    panel(fb, 8, 8, GFX_W - 16, GFX_H - 34);
    gfx_text(fb, &font_title, 20, 14, UI_TEXT, "Save sync", GFX_LEFT, 0);
    gfx_text(fb, &font_small, 20, 32, UI_MUTED, s->name, GFX_LEFT, GFX_W - 40);
    save_card(fb, 18, 50, 96, "DSi", s->local_date, s->local_size);
    save_card(fb, GFX_W - 18 - 96, 50, 96, "RomM", s->server_date, s->server_size);
    gfx_text(fb, &font_title, GFX_W / 2, 70, UI_FAINT, "\xE2\x86\x92", GFX_CENTER, 0);
    gfx_text(fb, &font_title, GFX_W / 2, 84, UI_FAINT, "\xE2\x86\x90", GFX_CENTER, 0);
    badge(fb, 18, 116, GFX_W - 36, s->kind, s->status);
    if (s->question)
        gfx_text(fb, &font_small, GFX_W / 2, 140, UI_TEXT, s->question, GFX_CENTER, GFX_W - 36);
    hint_line(fb, s->hint);
}

void ui_draw_page(uint16_t *fb, const char *title, const char *sub, const char *body,
                  const char *hint, int *scroll)
{
    background(fb);
    panel(fb, 8, 8, GFX_W - 16, GFX_H - 34);
    int y = 14;
    y += gfx_text_wrap(fb, &font_title, 20, y, GFX_W - 40, 2, UI_TEXT, title) *
         font_title.height;
    if (sub)
    {
        gfx_text(fb, &font_small, 20, y + 1, UI_MUTED, sub, GFX_LEFT, GFX_W - 40);
        y += font_small.height + 2;
    }
    if (!body)
    {
        hint_line(fb, hint);
        return;
    }
    y += 5;
    int rows = (GFX_H - 30 - y) / font_small.height;
    int total = gfx_text_wrap_scroll(NULL, &font_small, 0, 0, GFX_W - 46, 0, 0, 0, body);
    int max = total > rows ? total - rows : 0;
    if (*scroll > max)
        *scroll = max;
    if (*scroll < 0)
        *scroll = 0;
    gfx_text_wrap_scroll(fb, &font_small, 20, y, GFX_W - 46, *scroll, rows, UI_TEXT, body);
    if (max > 0)
    {
        // Scroll bar
        int th = rows * font_small.height, bx = GFX_W - 20;
        gfx_rrect(fb, bx, y, 4, th, 2, UI_BG2, 0, 0);
        int kh = th * rows / total;
        if (kh < 8)
            kh = 8;
        gfx_rrect(fb, bx, y + (th - kh) * *scroll / max, 4, kh, 2, UI_ACCENT_HI, 0, 0);
    }
    hint_line(fb, hint);
}

void ui_draw_list(uint16_t *fb, const char *title, const char *const *items, int n, int sel,
                  const char *hint)
{
    background(fb);
    gfx_text(fb, &font_title, 12, 8, UI_TEXT, title, GFX_LEFT, GFX_W - 24);
    int y = 30;
    for (int i = 0; i < n && y < GFX_H - 36; i++)
    {
        bool s = i == sel;
        gfx_rrect(fb, 8, y, GFX_W - 16, 22, 8, s ? UI_ACCENT : UI_PANEL, s ? UI_ACCENT : UI_LINE, 1);
        gfx_text(fb, &font_bold, 18, y + 5, s ? UI_WHITE : UI_TEXT, items[i], GFX_LEFT,
                 GFX_W - 40);
        y += 26;
    }
    hint_line(fb, hint);
}

void ui_draw_pairing(uint16_t *fb, const char *url, const char *code, int secs_left)
{
    background(fb);
    panel(fb, 8, 8, GFX_W - 16, GFX_H - 34);
    gfx_text(fb, &font_title, 20, 14, UI_TEXT, "Pair this DSi with RomM", GFX_LEFT, GFX_W - 40);
    gfx_text_wrap(fb, &font_small, 20, 36, GFX_W - 40, 3, UI_TEXT,
                  "Scan the QR code on the top screen and approve the device, or open "
                  "this address and enter the code:");
    gfx_text(fb, &font_bold, 20, 80, UI_ACCENT, url, GFX_LEFT, GFX_W - 40);
    gfx_rrect(fb, 48, 100, GFX_W - 96, 30, 9, GFX_RGB(226, 238, 252), UI_ACCENT, 1);
    gfx_text(fb, &font_title, GFX_W / 2, 108, UI_TEXT, code, GFX_CENTER, GFX_W - 100);
    char t[40];
    snprintf(t, sizeof(t), "Code valid for %d:%02d", secs_left / 60, secs_left % 60);
    gfx_text(fb, &font_small, GFX_W / 2, 136, UI_MUTED, t, GFX_CENTER, 0);
    hint_line(fb, "B: cancel");
}

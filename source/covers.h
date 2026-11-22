// SPDX-License-Identifier: MIT
#ifndef ROMM_COVERS_H
#define ROMM_COVERS_H

// Game artwork from RomM, decoded once and cached on the SD card in display
// format, one folder per platform: <base>/covers/<platform id>/
//   <rom id>.tl  strip image: 2D box art (ScreenScraper "box2d", stored by RomM
//                next to the cover), else the cover; whole picture, at most
//                TILE_S x TILE_S (.tc: from the cover, config art=cover)
//   <rom id>.tp  top screen image: the cover (path_cover_small) filling
//                COVER_W x COVER_H, else the box art fitted into it
//   <rom id>.tln / .tpn  no usable artwork for that image
// File: "RCV3", width, height (one byte each, 2 bytes padding), RGB15 pixels.

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "romindex.h"

#define COVER_W 90
#define COVER_H 120
#define TILE_S  64

enum { COV_UNKNOWN, COV_CACHED, COV_NONE, COV_MISSING };
enum { COV_TILE, COV_TOP };

// Strip image source: true = 2D box art first (config art=box2d), false = cover
extern bool cover_tile_box2d;

// Reads the platform's cover folder once (yielding to other threads) and sets
// the state of both images for each game (state arrays of n entries).
void cover_scan(const config_t *cfg, int platform_id, const rom_entry_t *e, int n,
                uint8_t *tile, uint8_t *top);

// Downloads, decodes and caches one image. Uses the network.
// Returns COV_CACHED, COV_NONE (no artwork) or COV_MISSING (try again later).
int cover_fetch(const config_t *cfg, int platform_id, const rom_entry_t *e, int kind);

// out: TILE_S^2 or COVER_W*COVER_H pixels; *w, *h: the image size
bool cover_load(const config_t *cfg, int platform_id, int rom_id, int kind, uint16_t *out,
                int *w, int *h);

// Background colour for transparent artwork (light/dark theme)
void cover_set_bg(bool dark);

// Streaming box-filter scaler: source rows in, RGB15 out (covers_img.c)
typedef struct
{
    int w, h;                // Source size
    int cx, cy, cw, ch;      // Source window used (crop)
    int dw, dh;              // Output size
    uint16_t *out;
    uint32_t *acc;           // r, g, b, count per output column
    uint16_t *xmap;          // Source column -> output column
    uint8_t *xuse;           // Source column sampled (window-relative)
    uint8_t *yuse;           // Source row sampled (window-relative)
    int oy;                  // Output row being accumulated
} rowscale_t;

// Rows/columns the scaler samples (at most 3 per output pixel and axis), so
// decoders can skip converting the others
static inline bool rs_row_wanted(const rowscale_t *rs, int y)
{
    return y >= rs->cy && y < rs->cy + rs->ch && rs->yuse[y - rs->cy];
}

void rs_geometry(int w, int h, bool fill, int maxw, int maxh, int *cx, int *cy, int *cw,
                 int *ch, int *dw, int *dh);
bool rs_init(rowscale_t *rs, int w, int h, bool fill, int maxw, int maxh, uint16_t *out);
// One source row (y ascending); bpp 3 = RGB, 4 = RGBA (alpha over the UI background)
void rs_row(rowscale_t *rs, int y, const uint8_t *px, int bpp);
void rs_finish(rowscale_t *rs);
void rs_free(rowscale_t *rs);

// Scaling of decoded RGB888 pixels (covers_img.c, also used by the host test)
void cover_fit(const uint8_t *rgb, int w, int h, uint16_t *out, int maxw, int maxh, int *ow,
               int *oh);
void cover_fill(const uint8_t *rgb, int w, int h, uint16_t *out, int dw, int dh);

#endif

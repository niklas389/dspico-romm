// SPDX-License-Identifier: MIT
#ifndef ROMM_GFX_H
#define ROMM_GFX_H

// 2D drawing on 256x192 RGB15 buffers in main RAM. gfx_present() copies them
// to the 16-bit bitmap backgrounds of both screens. The drawing functions
// need no hardware (also built on the host: tools/hosttest/ui_host.c).

#include <stdbool.h>
#include <stdint.h>

#include "font.h"

#define GFX_W 256
#define GFX_H 192

// RGB15 with the bitmap "opaque" bit (bit 15) set
#define GFX_RGB(r, g, b) ((uint16_t)(0x8000 | ((b) >> 3) << 10 | ((g) >> 3) << 5 | ((r) >> 3)))

typedef uint16_t gfx_color_t;

extern uint16_t *gfx_top; // Top screen back buffer
extern uint16_t *gfx_bot; // Bottom screen back buffer

enum { GFX_LEFT = 0, GFX_CENTER = 1, GFX_RIGHT = 2 };

// Hardware (DS only)
void gfx_init(void);        // Both screens: 16-bit bitmap background
void gfx_init_bottom(void); // Bottom screen again (after the text console)
void gfx_present(void);     // Waits for VBlank, copies the dirty buffers
void gfx_present_now(void); // Copies without waiting (progress during downloads)
void gfx_dirty(uint16_t *fb);

bool gfx_alloc(void);       // Back buffers only (host)

void gfx_fill(uint16_t *fb, int x, int y, int w, int h, gfx_color_t c);
void gfx_clear(uint16_t *fb, gfx_color_t c);
// Vertical gradient from c0 (top) to c1 (bottom)
void gfx_vgrad(uint16_t *fb, int x, int y, int w, int h, gfx_color_t c0, gfx_color_t c1);
// Rounded rectangle with anti-aliased corners. border: 0 for none.
void gfx_rrect(uint16_t *fb, int x, int y, int w, int h, int r, gfx_color_t fill,
               gfx_color_t border, int border_w);
// Rounded outline only
void gfx_rframe(uint16_t *fb, int x, int y, int w, int h, int r, gfx_color_t c, int t);
void gfx_circle(uint16_t *fb, int cx, int cy, int r, gfx_color_t c);
// Blends c over the rectangle (alpha 0..256)
void gfx_shade(uint16_t *fb, int x, int y, int w, int h, gfx_color_t c, int alpha);

// RGB15 image (w x h), scaled to dw x dh (nearest), clipped to a rounded rect
void gfx_blit(uint16_t *fb, int x, int y, int dw, int dh, const uint16_t *img, int w,
              int h, int radius);

// UTF-8 text. Returns the width drawn. maxw > 0: truncated with "...".
int gfx_text(uint16_t *fb, const font_t *f, int x, int y, gfx_color_t c, const char *s,
             int align, int maxw);
int gfx_text_width(const font_t *f, const char *s);
// Word-wrapped text in a box. Returns the number of lines drawn.
int gfx_text_wrap(uint16_t *fb, const font_t *f, int x, int y, int w, int max_lines,
                  gfx_color_t c, const char *s);

// Blend helper: a over b, alpha 0..256
gfx_color_t gfx_mix(gfx_color_t a, gfx_color_t b, int alpha);

#endif

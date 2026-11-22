// SPDX-License-Identifier: MIT
#ifndef ROMM_UI_H
#define ROMM_UI_H

// Screens of the DSi-menu-style interface. Each ui_draw_* function only
// draws into a back buffer (gfx.h) from the data it is given; main.c owns the
// state and input. Also built on the host to render screenshots
// (tools/hosttest/ui_host.c).

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

// Theme (fixed blue)
#define UI_BG        GFX_RGB(236, 238, 241)
#define UI_BG2       GFX_RGB(222, 226, 232)
#define UI_PANEL     GFX_RGB(255, 255, 255)
#define UI_LINE      GFX_RGB(200, 204, 212)
#define UI_TEXT      GFX_RGB(32, 34, 42)
#define UI_MUTED     GFX_RGB(104, 108, 120)
#define UI_FAINT     GFX_RGB(170, 174, 184)
#define UI_ACCENT    GFX_RGB(52, 122, 204)
#define UI_ACCENT_HI GFX_RGB(96, 160, 232)
#define UI_GREEN     GFX_RGB(52, 150, 78)
#define UI_GREEN_BG  GFX_RGB(222, 244, 226)
#define UI_ORANGE    GFX_RGB(214, 130, 36)
#define UI_ORANGE_BG GFX_RGB(255, 240, 220)
#define UI_RED       GFX_RGB(204, 60, 56)
#define UI_RED_BG    GFX_RGB(252, 228, 226)
#define UI_WHITE     GFX_RGB(255, 255, 255)

enum { UI_INFO, UI_OK, UI_WARN, UI_ERROR }; // Kinds (colours) of messages

// Placeholder colour for a name (stable hash)
gfx_color_t ui_name_color(const char *name);
// Short label for a placeholder tile: initials of the first words
void ui_initials(const char *name, char *out, int len);

// --- Top screen ---------------------------------------------------------------

void ui_top_header(uint16_t *fb, const char *left, const char *right);

typedef struct
{
    const char *name;
    const char *platform;
    const char *regions;
    const char *genres;
    const char *players;
    const char *summary;     // May be NULL
    int year;
    uint64_t size;
    bool on_sd;
    bool archive;            // Unpacked after download
    const char *save;        // Save status text, NULL if not applicable
    int save_kind;           // UI_*
    const uint16_t *cover;   // NULL: placeholder
    int cover_w, cover_h;
    const char *clock;
} ui_game_t;

void ui_draw_game_top(uint16_t *fb, const ui_game_t *g);

typedef struct
{
    const char *name;
    const char *label;       // Short tile label, e.g. "NDS"
    const char *folder;      // roms/<folder>/
    int games;
    int on_sd;               // -1: unknown
    bool hidden;
    const char *note;        // e.g. last save sync result, may be NULL
    const char *clock;
} ui_platform_t;

void ui_draw_platform_top(uint16_t *fb, const ui_platform_t *p);

// Start-up / about screen: title and version
void ui_draw_splash_top(uint16_t *fb, const char *version, const char *line);

// --- Bottom screen --------------------------------------------------------------

#define UI_TILE_ON_SD   1
#define UI_TILE_DIMMED  2
#define UI_TILE_WARN    4

typedef struct
{
    const char *name;
    const char *label;       // Placeholder text
    gfx_color_t color;       // Placeholder colour
    const uint16_t *img;     // NULL: placeholder
    int img_w, img_h;
    uint8_t flags;           // UI_TILE_*
} ui_tile_t;

typedef struct
{
    const char *title;
    const char *subtitle;
    const ui_tile_t *tiles;
    int count;
    int sel;
    int pos_q8;              // Scroll position (index * 256), animated
    const char *caption2;    // Second line under the name, may be NULL
    const bool *letters;     // 27 entries ('#', 'A'..'Z'), NULL: no letter bar
    int letter;              // Current letter index
    const char *hint;
} ui_strip_t;

void ui_draw_strip(uint16_t *fb, const ui_strip_t *s);

// Letter index of a name: 0 = '#', 1..26 = A..Z (accents folded)
int ui_letter_of(const char *name);

typedef struct
{
    const char *text;
    int state;               // 0 waiting, 1 working, 2 done, 3 failed
    const char *detail;
} ui_step_t;

void ui_draw_steps(uint16_t *fb, const char *title, const ui_step_t *steps, int n,
                   const char *hint);

void ui_draw_message(uint16_t *fb, int kind, const char *title, const char *body,
                     const char *hint);

typedef struct
{
    const char *title;       // "Downloading"
    const char *name;
    int permille;            // -1: indeterminate
    const char *line1;
    const char *line2;
    const char *phases[3];   // e.g. "Download", "Unpack"; NULL-terminated
    int phase;
    const char *hint;
    int kind;                // UI_* colour of the bar
} ui_progress_t;

void ui_draw_progress(uint16_t *fb, const ui_progress_t *p);

typedef struct
{
    const char *name;
    const char *local_date;   // NULL: no save on the DSi
    const char *local_size;
    const char *server_date;  // NULL: no save in RomM
    const char *server_size;
    const char *status;
    int kind;
    const char *question;     // May be NULL
    const char *hint;
} ui_savesync_t;

void ui_draw_savesync(uint16_t *fb, const ui_savesync_t *s);

// Text page (game details): title, body text, hint. *scroll: first body line
// (clamped here).
void ui_draw_page(uint16_t *fb, const char *title, const char *sub, const char *body,
                  const char *hint, int *scroll);

// Vertical list (hidden menu, settings)
void ui_draw_list(uint16_t *fb, const char *title, const char *const *items, int n, int sel,
                  const char *hint);

// Pairing instructions (QR code on the top screen)
void ui_draw_pairing(uint16_t *fb, const char *url, const char *code, int secs_left);

#endif

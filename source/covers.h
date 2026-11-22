// SPDX-License-Identifier: MIT
#ifndef ROMM_COVERS_H
#define ROMM_COVERS_H

// Game artwork from RomM, decoded once and cached on the SD card in display
// format: <base>/covers/<rom id>.cv2
//   "RCV2", top w, top h, tile w, tile h (one byte each), then the top screen
//   image (at most COVER_W x COVER_H) and the strip image (at most TILE_S x
//   TILE_S), RGB15, whole picture with its aspect ratio.
// Source: the 2D box art (ScreenScraper "box2d", stored by RomM next to the
// cover) if the server has it, else the cover (path_cover_small).
// <rom id>.nn2 marks a game without usable artwork.

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "romindex.h"

#define COVER_W 90
#define COVER_H 120
#define TILE_S  64

enum { COV_UNKNOWN, COV_CACHED, COV_NONE, COV_MISSING };

// State of the cache for one ROM (stat on the SD card)
int cover_state(const config_t *cfg, const rom_entry_t *e);

// Downloads, decodes and caches the cover. Uses the network.
// Returns COV_CACHED or COV_NONE.
int cover_fetch(const config_t *cfg, const rom_entry_t *e);

// out: TILE_S^2 / COVER_W*COVER_H pixels; *w, *h: the image size
bool cover_load_tile(const config_t *cfg, int rom_id, uint16_t *out, int *w, int *h);
bool cover_load_top(const config_t *cfg, int rom_id, uint16_t *out, int *w, int *h);

// Scales decoded RGB888 pixels to fit maxw x maxh (aspect ratio kept)
void cover_fit(const uint8_t *rgb, int w, int h, uint16_t *out, int maxw, int maxh, int *ow,
               int *oh);

#endif

// SPDX-License-Identifier: MIT
#ifndef ROMM_COVERS_H
#define ROMM_COVERS_H

// Cover art from RomM (path_cover_small), decoded once and cached on the SD
// card in display format: <base>/covers/<rom id>.cov
//   "RCV1", then COVER_W x COVER_H RGB15 (top screen), then TILE_S x TILE_S
//   RGB15 (icon strip, square crop of the cover).
// <rom id>.none marks a cover that is missing or could not be decoded.

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

bool cover_load_tile(const config_t *cfg, int rom_id, uint16_t *out); // TILE_S^2
bool cover_load_top(const config_t *cfg, int rom_id, uint16_t *out);  // COVER_W*COVER_H

// Converts decoded RGB888 pixels into both images (also used by the host test)
void cover_convert(const uint8_t *rgb, int w, int h, uint16_t *top, uint16_t *tile);

#endif

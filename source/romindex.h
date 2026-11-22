// SPDX-License-Identifier: MIT
#ifndef ROMM_ROMINDEX_H
#define ROMM_ROMINDEX_H

// Local ROM index per platform, stored on the SD card:
//   <base>/index/<platform_id>.tsv   one line per ROM (fields below)
//   <base>/meta/<rom_id>.txt         summary and companies (detail view)
//
// The first sync downloads the platform's ROM list without provider metadata
// (~5 KB per ROM instead of ~100 KB). Later syncs only request ROMs updated
// after the newest updated_at seen, and drop ROMs that no longer exist
// (/api/roms/identifiers).

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "net.h"

typedef struct
{
    int id;
    uint32_t size;
    int year;            // First release year, 0 if unknown
    char *name;
    char *fs_name;
    char *ext;           // fs_extension
    char *updated;       // updated_at (ISO 8601)
    char *regions;       // comma-separated
    char *genres;        // comma-separated
    char *players;       // e.g. "1-4"
    char *cover;         // path_cover_small (server-relative), may be empty
} rom_entry_t;

typedef struct
{
    int platform_id;
    int count;
    int cap;
    rom_entry_t *e;
    char watermark[40];  // Newest updated_at in the index
} rom_index_t;

typedef struct
{
    int fetched;         // ROM records received
    int removed;         // Dropped because they no longer exist on the server
    int requests;
    uint32_t ms;
    bool full;           // Full sync (no index before)
    char error[128];
} idx_sync_result_t;

typedef void (*idx_progress_fn)(int fetched, int total);

void idx_init(rom_index_t *idx, int platform_id);
void idx_free(rom_index_t *idx);

bool idx_load(const config_t *cfg, rom_index_t *idx);
bool idx_save(const config_t *cfg, const rom_index_t *idx);

// Updates idx from the server (full when idx is empty), then saves it.
bool idx_sync(const config_t *cfg, rom_index_t *idx, idx_progress_fn cb,
              idx_sync_result_t *res);

// Reads <base>/meta/<rom_id>.txt into buf (NUL-terminated). False if absent.
bool idx_meta(const config_t *cfg, int rom_id, char *buf, int len);

#endif

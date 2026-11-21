// SPDX-License-Identifier: MIT
#ifndef ROMM_DOWNLOAD_H
#define ROMM_DOWNLOAD_H

// ROM downloads into the SD card library: <drive>/roms/<folder>/<file>.
//
// The data goes to "<file>.part" first and is renamed when complete. The
// file is allocated up front; on a DSpico the sectors are written with the
// interrupt-driven DMA writer (sdraw.c, config key sdraw), otherwise through
// the DLDI driver. Connection drops are resumed with HTTP Range requests.

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "net.h"

typedef struct
{
    bool ok;
    bool cancelled;
    bool raw;               // Written with the DSpico DMA writer
    int attempts;           // HTTP requests made (1 = no resume needed)
    uint64_t bytes;
    uint32_t ms;
    char path[CFG_PATH_MAX + 256];
    char error[160];
} download_result_t;

// Library folder for a RomM platform (slug and fs_slug from /api/platforms).
const char *download_folder(const char *slug, const char *fs_slug);

// Full path of a ROM in the library ("fat:/roms/nds/Game.nds").
void download_target(const config_t *cfg, const char *folder, const char *fs_name,
                     char *out, int out_len);

bool download_exists(const config_t *cfg, const char *folder, const char *fs_name,
                     uint64_t *size);

bool download_rom(const config_t *cfg, int rom_id, const char *fs_name, uint64_t size,
                  const char *folder, net_progress_fn cb, void *ctx,
                  download_result_t *res);

#endif

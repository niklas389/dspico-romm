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
#include "unzip.h"

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

// Archives (.zip): the archive is downloaded into the library folder, then
// the ROM inside is extracted to "<archive stem>.<rom ext>" and the archive
// deleted. If extraction fails for another reason than corrupt data, the
// archive stays so a retry needs no new download.
typedef struct
{
    bool ok;
    bool cancelled;
    bool raw;
    bool zip_removed;
    int entries;
    uint64_t in_bytes, out_bytes;
    uint32_t ms;
    zip_stats_t st;
    char entry[256];
    char path[CFG_PATH_MAX + 256];
    char error[160];
} extract_result_t;

bool download_is_archive(const char *fs_name);

// i-th possible library file name of a RomM file (for an archive: the stem
// with each ROM extension of the folder). False when there are no more.
bool download_local_name(const char *folder, const char *fs_name, int i,
                         char *out, int out_len);

// Finds the library copy of a RomM file (archive aware). name may be NULL.
bool download_local(const config_t *cfg, const char *folder, const char *fs_name,
                    char *name, int name_len, uint64_t *size);

bool download_extract(const config_t *cfg, const char *folder, const char *zip_name,
                      zip_progress_fn cb, void *ctx, extract_result_t *res);

// Free space on the library's drive.
bool download_free_space(const config_t *cfg, uint64_t *bytes);

// Space check before a download: the file, plus for an archive the ROM
// inside (its size comes from the archive's central directory, read with a
// Range request, or from the local archive when have_zip). False if it
// doesn't fit; also true when the free space can't be determined.
typedef struct
{
    uint64_t need;       // Bytes needed at the peak (archive + ROM)
    uint64_t free;
    uint64_t rom_size;   // ROM inside the archive, 0 if not an archive/unknown
    char note[128];
} space_check_t;

bool download_space_check(const config_t *cfg, int rom_id, const char *folder,
                          const char *fs_name, uint64_t size, bool have_zip,
                          space_check_t *sc);

#endif

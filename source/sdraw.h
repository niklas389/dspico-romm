// SPDX-License-Identifier: MIT
#ifndef ROMM_SDRAW_H
#define ROMM_SDRAW_H

// Direct SD sector writes through the DSpico card commands (see
// LNH-team/dspico-firmware docs/commands.md and pico-loader's
// DSPicoWriteSdSectorsPatchCode.s), bypassing the DLDI driver.
//
// Only ever used on sectors that belong to a file which was allocated
// beforehand (sdraw_map), so the FAT and directory are never touched by these
// writes. Speed test only for now.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct
{
    uint32_t lba;      // First sector
    uint32_t count;    // Number of sectors
} sdraw_extent_t;

// True if the active DLDI driver is the DSpico's (friendly name).
bool sdraw_is_dspico(void);
const char *sdraw_dldi_name(void);

// Extends the open file f (opened for writing) to size bytes without writing
// data, flushes FAT and directory, and returns its sectors as extents (in
// file order). Returns the number of extents, or -1.
int sdraw_map(FILE *f, uint32_t size, sdraw_extent_t *ext, int max_ext);

// Writes count sectors from src (word aligned) to lba. dma: transfer the data
// with ARM9 DMA channel 2 (card start timing) instead of CPU stores.
// Returns false on a timeout. Drops libnds' cached copies of these sectors.
bool sdraw_write(uint32_t lba, const void *src, uint32_t count, bool dma);

#endif

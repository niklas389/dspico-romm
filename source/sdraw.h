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

// Interrupt-driven sequential writes and reads with DMA (card transfer IRQ +
// timer 1). Operations are queued and run in order; the next one starts from
// the IRQ handler. sdraw_async_init() also makes the DLDI driver wait while
// operations are pending.
enum { SDRAW_OP_QUEUED = 1, SDRAW_OP_RUNNING, SDRAW_OP_DONE, SDRAW_OP_ERROR };

typedef struct sdraw_op
{
    bool write;
    uint32_t lba;
    uint32_t count;            // Sectors
    void *buf;                 // 32-byte aligned (cache lines)
    volatile int state;        // SDRAW_OP_*
    struct sdraw_op *next;     // Queue link (internal)
} sdraw_op_t;

bool sdraw_async_init(void);
// Queues op (fill write/lba/count/buf). The buffer must not be touched until
// op->state is DONE or ERROR. False if op is invalid.
bool sdraw_op_submit(sdraw_op_t *op);
static inline bool sdraw_op_pending(const sdraw_op_t *op)
{
    return op->state == SDRAW_OP_QUEUED || op->state == SDRAW_OP_RUNNING;
}
bool sdraw_async_busy(void);     // Any operation queued or running
typedef struct
{
    uint32_t irqs, polls;
    uint64_t irq_ticks;          // BUS_CLOCK ticks spent in the IRQ handlers
    uint32_t avg_read_ticks;     // Usual wait until a sector is ready
    uint32_t avg_write_ticks;
} sdraw_stats_t;
void sdraw_async_stats_ex(sdraw_stats_t *st); // Cumulative
void sdraw_async_stats(uint32_t *irqs, uint32_t *polls);

#endif

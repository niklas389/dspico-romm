// SPDX-License-Identifier: MIT
#ifndef ROMM_SDWRITER_H
#define ROMM_SDWRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Background SD writer. Downloaded data is copied into a RAM ring buffer and a
// separate cothread writes it to the file. Cothreads only switch when a thread
// waits, so the writer runs exactly while the main thread is blocked waiting
// for network data -- SD writes overlap with network waits instead of adding
// to them.

typedef struct sdwriter sdwriter_t;

// ring_size: RAM buffer size; chunk: bytes per fwrite() call.
sdwriter_t *sdw_open(FILE *fp, size_t ring_size, size_t chunk);

// Copies data into the ring. Blocks (yields) while the ring is full.
bool sdw_write(sdwriter_t *w, const void *data, size_t len);

// Zero-copy variant of sdw_write(): returns the contiguous free space at the
// head of the ring (waits/yields while the ring is full) and its size in
// *avail. Fill up to *avail bytes, then call sdw_commit() with the count.
// Returns NULL if a write failed.
void *sdw_reserve(sdwriter_t *w, size_t *avail);
void sdw_commit(sdwriter_t *w, size_t n);

// Waits until everything is written, stops the thread, frees the writer.
// Does not close fp. Returns false if any write failed.
bool sdw_close(sdwriter_t *w);

// Statistics for the speed test (BUS_CLOCK ticks).
uint64_t sdw_write_ticks(const sdwriter_t *w); // Time inside fwrite()
uint64_t sdw_stall_ticks(const sdwriter_t *w); // Producer waited for space

#endif

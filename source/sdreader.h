// SPDX-License-Identifier: MIT
#ifndef ROMM_SDREADER_H
#define ROMM_SDREADER_H

// Read-ahead of a file's sectors with the DSpico's interrupt-driven DMA reads
// (sdraw_op_*): a few slots are always queued, so the card keeps reading
// while the CPU works on earlier data; no thread is needed because the IRQ
// handler starts the next queued operation itself. Read-only: the file must
// not change while it is open.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdraw.h"

typedef struct sdreader sdreader_t;

// Bytes [offset, offset + len) of the file with these extents (sdraw_map).
sdreader_t *sdr_open(const sdraw_extent_t *ext, int n_ext, uint64_t offset,
                     uint64_t len);

// Next contiguous bytes (waits, yielding, until they arrived): 1 data,
// 0 end, -1 read error. Valid until sdr_consume().
int sdr_get(sdreader_t *r, const uint8_t **p, size_t *n);
void sdr_consume(sdreader_t *r, size_t n);

// Waits for queued reads, frees the reader.
void sdr_close(sdreader_t *r);

#endif

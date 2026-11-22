// SPDX-License-Identifier: MIT
#ifndef ROMM_CRC32FAST_H
#define ROMM_CRC32FAST_H

#include <stddef.h>
#include <stdint.h>

// CRC-32 (zip/zlib polynomial), same results as zlib's crc32(): slicing-by-4
// with the 4 KB of tables and the code in ITCM, so table lookups never miss
// the ARM9's 4 KB data cache (zlib's 5 KB of tables in main RAM do).
uint32_t crc32_fast(uint32_t crc, const void *data, size_t len);

// Same code and tables in main RAM (for the speed test).
uint32_t crc32_fast_ram(uint32_t crc, const void *data, size_t len);

#endif

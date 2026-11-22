// SPDX-License-Identifier: MIT
#ifndef ROMM_GHASH_H
#define ROMM_GHASH_H

// GHASH (the GCM authenticator) with an 8-bit table (Shoup's method, as in
// OpenSSL's gcm_gmult_8bit): 16 table lookups per 16-byte block. The 4 KB
// table and the code are in ITCM. One key at a time (static table).

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t x[4];      // Running value, big-endian words
} ghash_t;

void ghash_init(ghash_t *g, const uint8_t h[16]);
// Whole 16-byte blocks; a partial last block is zero-padded.
void ghash_update(ghash_t *g, const uint8_t *data, size_t len);
void ghash_final(const ghash_t *g, uint8_t out[16]);

#endif

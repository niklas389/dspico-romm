// SPDX-License-Identifier: MIT
//
// GHASH with an 8-bit table (see ghash.h).

#include "ghash.h"

#include <string.h>

#ifdef __NDS__
#include <nds/ndstypes.h>
#define GH_CODE  ITCM_CODE ARM_CODE
#define GH_TABLE ITCM_BSS
#else
#define GH_CODE
#define GH_TABLE
#endif

// m[i] = i * H in GCM's bit order; rem[r] = reduction of the 8 bits shifted
// out at the bottom, already placed in the top 16 bits.
static uint32_t m[256][4] GH_TABLE __attribute__((aligned(32)));
static uint16_t rem[256] GH_TABLE;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// v * x: shift right by one bit (GCM order), reduce with 0xE1 << 120
static void mulx(uint32_t v[4])
{
    uint32_t lsb = v[3] & 1;
    v[3] = (v[3] >> 1) | (v[2] << 31);
    v[2] = (v[2] >> 1) | (v[1] << 31);
    v[1] = (v[1] >> 1) | (v[0] << 31);
    v[0] = (v[0] >> 1) ^ (lsb ? 0xE1000000u : 0);
}

void ghash_init(ghash_t *g, const uint8_t h[16])
{
    memset(g, 0, sizeof(*g));
    uint32_t v[4] = { be32(h), be32(h + 4), be32(h + 8), be32(h + 12) };
    memset(m[0], 0, sizeof(m[0]));
    for (int i = 0x80; i > 0; i >>= 1)
    {
        memcpy(m[i], v, sizeof(v));
        mulx(v);
    }
    for (int i = 2; i < 256; i <<= 1)
        for (int j = 1; j < i; j++)
            for (int k = 0; k < 4; k++)
                m[i + j][k] = m[i][k] ^ m[j][k];

    for (int r = 0; r < 256; r++)
    {
        uint32_t t[4] = { 0, 0, 0, (uint32_t)r };
        for (int s = 0; s < 8; s++)
            mulx(t);
        rem[r] = (uint16_t)(t[0] >> 16); // Only the top 16 bits can be set
    }
}

GH_CODE static void gmult(uint32_t x[4])
{
    // Bytes of X from the last to the first (big-endian words)
    uint32_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
    for (int i = 15;; i--)
    {
        uint32_t b = (x[i >> 2] >> (8 * (3 - (i & 3)))) & 0xFF;
        const uint32_t *t = m[b];
        z0 ^= t[0];
        z1 ^= t[1];
        z2 ^= t[2];
        z3 ^= t[3];
        if (i == 0)
            break;
        uint32_t r = z3 & 0xFF;
        z3 = (z3 >> 8) | (z2 << 24);
        z2 = (z2 >> 8) | (z1 << 24);
        z1 = (z1 >> 8) | (z0 << 24);
        z0 = (z0 >> 8) ^ ((uint32_t)rem[r] << 16);
    }
    x[0] = z0;
    x[1] = z1;
    x[2] = z2;
    x[3] = z3;
}

GH_CODE void ghash_update(ghash_t *g, const uint8_t *data, size_t len)
{
    while (len >= 16)
    {
        g->x[0] ^= be32(data);
        g->x[1] ^= be32(data + 4);
        g->x[2] ^= be32(data + 8);
        g->x[3] ^= be32(data + 12);
        gmult(g->x);
        data += 16;
        len -= 16;
    }
    if (len)
    {
        uint8_t last[16] = { 0 };
        memcpy(last, data, len);
        ghash_update(g, last, 16);
    }
}

void ghash_final(const ghash_t *g, uint8_t out[16])
{
    for (int k = 0; k < 4; k++)
        for (int b = 0; b < 4; b++)
            out[4 * k + b] = (uint8_t)(g->x[k] >> (24 - 8 * b));
}

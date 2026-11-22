// SPDX-License-Identifier: MIT
//
// Slicing-by-4 CRC-32, included by crc32fast.itcm.arm.c (tables and code in
// ITCM) and crc32fast_ram.arm.c (main RAM copy for the speed test). The
// includer defines CRC_FN, CRC_CODE and CRC_TABLE.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t tab[4][256] CRC_TABLE __attribute__((aligned(32)));
static bool tab_ready CRC_TABLE;

static void make_tables(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        tab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 4; t++)
            tab[t][i] = (tab[t - 1][i] >> 8) ^ tab[0][tab[t - 1][i] & 0xFF];
    tab_ready = true;
}

CRC_CODE uint32_t CRC_FN(uint32_t crc, const void *data, size_t len)
{
    if (!tab_ready)
        make_tables();

    const uint32_t *t0 = tab[0], *t1 = tab[1], *t2 = tab[2], *t3 = tab[3];
    const uint8_t *p = data;
    crc = ~crc;

    while (len && ((uintptr_t)p & 3))
    {
        crc = t0[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
        len--;
    }

    const uint32_t *w = (const uint32_t *)p;
    while (len >= 8) // Little-endian: two words per iteration
    {
        uint32_t a = crc ^ w[0], b = w[1];
        crc = t3[a & 0xFF] ^ t2[(a >> 8) & 0xFF] ^ t1[(a >> 16) & 0xFF] ^ t0[a >> 24];
        crc ^= b;
        crc = t3[crc & 0xFF] ^ t2[(crc >> 8) & 0xFF] ^ t1[(crc >> 16) & 0xFF] ^
              t0[crc >> 24];
        w += 2;
        len -= 8;
    }
    p = (const uint8_t *)w;

    while (len--)
        crc = t0[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

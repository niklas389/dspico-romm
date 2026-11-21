// SPDX-License-Identifier: MIT
//
// Fast memcpy for the ARM9, replacing the libnds one for every library via
// the linker (-Wl,--wrap=memcpy etc., see the Makefile).
//
// Profiling showed ~40% of ARM9 time in libnds' __ndsabi_memcpy1/2, the byte
// and halfword fallbacks used when source and destination have different
// alignment. That is the normal case here: Mbed TLS keeps decrypted TLS data
// at in_buf + 13, lwIP packet payloads sit at odd halfword offsets, etc.
//
// This version always copies whole words: if the source is misaligned
// relative to the destination it loads aligned source words and merges them
// with shifts (little-endian). It runs from ITCM in ARM mode, so it is
// unaffected by instruction cache misses.
//
// Over-read note: the shift-merge path may read up to 3 bytes beyond the last
// source byte, but never beyond the aligned word that contains it, which is
// always safe on the DS (no MMU, memory protection granularity >= 4 KB).

#include <stddef.h>
#include <stdint.h>

#ifdef __NDS__
#include <nds/ndstypes.h>
#define FASTCOPY_ATTR ITCM_CODE ARM_CODE __attribute__((noinline))
#else
#define FASTCOPY_ATTR __attribute__((noinline))
#endif

#define MERGE_LOOP(SH)                                                   \
    do {                                                                 \
        uint32_t cur = *ws++;                                            \
        while (nwords >= 4)                                              \
        {                                                                \
            uint32_t a = ws[0], b = ws[1], c = ws[2], e = ws[3];        \
            d[0] = (cur >> (SH)) | (a << (32 - (SH)));                   \
            d[1] = (a >> (SH)) | (b << (32 - (SH)));                     \
            d[2] = (b >> (SH)) | (c << (32 - (SH)));                     \
            d[3] = (c >> (SH)) | (e << (32 - (SH)));                     \
            cur = e;                                                     \
            ws += 4;                                                     \
            d += 4;                                                      \
            nwords -= 4;                                                 \
        }                                                                \
        while (nwords > 0)                                               \
        {                                                                \
            uint32_t a = *ws++;                                          \
            *d++ = (cur >> (SH)) | (a << (32 - (SH)));                   \
            cur = a;                                                     \
            nwords--;                                                    \
        }                                                                \
    } while (0)

FASTCOPY_ATTR void *fast_memcpy(void *restrict dst, const void *restrict src,
                                size_t n)
{
    uint8_t *d8 = dst;
    const uint8_t *s8 = src;

    if (n < 16)
    {
        while (n--)
            *d8++ = *s8++;
        return dst;
    }

    // Align the destination to a word boundary.
    while ((uintptr_t)d8 & 3)
    {
        *d8++ = *s8++;
        n--;
    }

    uint32_t *d = (uint32_t *)d8;
    size_t nwords = n >> 2;
    size_t tail = n & 3;
    unsigned int k = (uintptr_t)s8 & 3;

    if (k == 0)
    {
        const uint32_t *s = (const uint32_t *)s8;
        while (nwords >= 8)
        {
            uint32_t a = s[0], b = s[1], c = s[2], e = s[3];
            uint32_t f = s[4], g = s[5], h = s[6], i = s[7];
            d[0] = a; d[1] = b; d[2] = c; d[3] = e;
            d[4] = f; d[5] = g; d[6] = h; d[7] = i;
            s += 8;
            d += 8;
            nwords -= 8;
        }
        while (nwords--)
            *d++ = *s++;
        s8 = (const uint8_t *)s;
    }
    else
    {
        const uint32_t *ws = (const uint32_t *)(s8 - k);
        size_t copied = nwords * 4;
        if (k == 1)
            MERGE_LOOP(8);
        else if (k == 2)
            MERGE_LOOP(16);
        else
            MERGE_LOOP(24);
        s8 += copied;
    }

    d8 = (uint8_t *)d;
    while (tail--)
        *d8++ = *s8++;

    return dst;
}

#ifdef __NDS__
// Linker wrappers. __aeabi_memcpy4/8 promise aligned pointers, but going
// through the same function costs only a few cycles and keeps this simple.
void *__wrap_memcpy(void *dst, const void *src, size_t n)
{
    return fast_memcpy(dst, src, n);
}

void __wrap___aeabi_memcpy(void *dst, const void *src, size_t n)
{
    fast_memcpy(dst, src, n);
}

void __wrap___aeabi_memcpy4(void *dst, const void *src, size_t n)
{
    fast_memcpy(dst, src, n);
}

void __wrap___aeabi_memcpy8(void *dst, const void *src, size_t n)
{
    fast_memcpy(dst, src, n);
}
#endif

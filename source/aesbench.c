// SPDX-License-Identifier: MIT
//
// Feasibility test for TLS_AES_128_GCM with the DSi AES engine (see
// aesbench.h): the engine does AES-CTR on the ARM7 (patches/
// dswifi-arm7-aes-test.patch), the ARM9 would only do GHASH.

#include "aesbench.h"

#include <malloc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>
#include <mbedtls/aes.h>

#include "ghash.h"
#include "log.h"
#include "prof.h"

#ifdef DSPICO_DSWIFI

// Must match aes_job_t in the ARM7 test (aes_test.twl.c)
typedef struct
{
    u8 key[16];
    u8 iv[16];
    u32 cnt_extra;
    u32 flags;          // 1: reverse key bytes, 2: reverse IV bytes, 4: NDMA
    const u32 *src;
    u32 *dst;
    u32 blocks;
    u32 lines;
    volatile u32 done;
} aes_job_t;

#define BENCH_BYTES (256 * 1024)

static uint32_t bswap(uint32_t v)
{
    return __builtin_bswap32(v);
}

// Runs a job on the ARM7. Returns ARM9 wall time in BUS_CLOCK ticks, 0 on timeout.
static uint64_t run_job(aes_job_t *job, size_t bytes)
{
    job->done = 0;
    DC_FlushRange(job, sizeof(*job));
    DC_FlushRange(job->src, bytes);
    DC_InvalidateRange(job->dst, bytes);

    volatile aes_job_t *uj = memUncached(job);
    uint64_t t0 = prof_ticks();
    fifoSendAddress(FIFO_USER_07, job);
    uint64_t limit = (uint64_t)BUS_CLOCK * 2; // 2 s
    while (uj->done == 0 && prof_ticks() - t0 < limit)
        ;
    uint64_t dt = prof_ticks() - t0;
    DC_InvalidateRange(job->dst, bytes);
    return uj->done == 1 ? (dt ? dt : 1) : 0;
}

static uint32_t kbps(size_t bytes, uint64_t ticks)
{
    return ticks ? (uint32_t)((uint64_t)bytes * BUS_CLOCK / 1024 / ticks) : 0;
}

void aes_bench(void)
{
    if (!isDSiMode())
        return;
    log_printf("DSi AES engine (AES-128-CTR on the ARM7):\n");

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    aes_job_t *job = memalign(32, sizeof(aes_job_t));
    u32 *src = memalign(32, BENCH_BYTES);
    u32 *dst = memalign(32, BENCH_BYTES);
    u8 *ref = memalign(32, BENCH_BYTES);
    if (!job || !src || !dst || !ref)
    {
        log_printf("  out of memory\n");
        goto out;
    }

    // Key and GCM-style counter block: 12-byte IV || 32-bit counter 2
    u8 key[16], iv[16];
    for (int i = 0; i < 16; i++)
        key[i] = (u8)(rand() >> 7);
    for (int i = 0; i < 12; i++)
        iv[i] = (u8)(rand() >> 7);
    iv[12] = iv[13] = iv[14] = 0;
    iv[15] = 2;

    // Software reference keystream (AES-CTR, 128-bit big-endian counter;
    // identical to GCM's 32-bit increment while it does not wrap)
    mbedtls_aes_setkey_enc(&aes, key, 128);
    memset(ref, 0, BENCH_BYTES);
    {
        size_t off = 0;
        u8 nc[16], sb[16];
        memcpy(nc, iv, 16);
        mbedtls_aes_crypt_ctr(&aes, BENCH_BYTES, &off, nc, sb, ref, ref);
    }

    // 1. Find the register conventions: 4 blocks of zeros -> keystream
    static const u32 extra[4] = { 0, 1u << 22, 1u << 23, (1u << 22) | (1u << 23) };
    int found = -1;
    for (int v = 0; v < 32 && found < 0; v++)
    {
        memset(job, 0, sizeof(*job));
        memcpy(job->key, key, 16);
        memcpy(job->iv, iv, 16);
        job->flags = v & 3;
        job->cnt_extra = extra[(v >> 2) & 3];
        job->src = src;
        job->dst = dst;
        job->blocks = 4;
        memset(src, 0, 64);
        if (!run_job(job, 64))
        {
            log_printf("  no answer from the ARM7 (variant %d)\n", v);
            goto out;
        }
        bool swap = v & 16;
        bool ok = true;
        for (int w = 0; w < 16 && ok; w++)
        {
            u32 got = swap ? bswap(dst[w]) : dst[w];
            u32 want;
            memcpy(&want, ref + 4 * w, 4);
            ok = got == want;
        }
        if (ok)
            found = v;
    }
    if (found < 0)
    {
        log_printf("  no register variant matches software AES-CTR (32 tried)\n");
        log_printf("  first word hw %08lX sw %08lX\n", (unsigned long)dst[0],
                   (unsigned long)((u32 *)ref)[0]);
        goto out;
    }
    bool out_swap = found & 16;
    log_printf("  match: key %s, IV %s, CNT |= %08lX, output %s\n",
               (found & 1) ? "reversed" : "as is", (found & 2) ? "reversed" : "as is",
               (unsigned long)extra[(found >> 2) & 3],
               out_swap ? "byte-swapped" : "as is");

    // 2. Real data through both paths; input byte order found the same way
    u8 *plain = (u8 *)malloc(BENCH_BYTES);
    if (!plain)
        goto out;
    for (size_t i = 0; i < BENCH_BYTES; i++)
        plain[i] = (u8)(rand() >> 5);
    // ref := plain XOR keystream (expected ciphertext)
    for (size_t i = 0; i < BENCH_BYTES; i++)
        ref[i] ^= plain[i];

    for (int path = 0; path < 2; path++)
    {
        int ok_swap = -1;
        uint64_t best = 0;
        for (int in_swap = 0; in_swap < 2 && ok_swap < 0; in_swap++)
        {
            memcpy(src, plain, BENCH_BYTES);
            if (in_swap)
                for (size_t w = 0; w < BENCH_BYTES / 4; w++)
                    src[w] = bswap(src[w]);
            memset(job, 0, sizeof(*job));
            memcpy(job->key, key, 16);
            memcpy(job->iv, iv, 16);
            job->flags = (found & 3) | (path ? 4 : 0);
            job->cnt_extra = extra[(found >> 2) & 3];
            job->src = src;
            job->dst = dst;
            job->blocks = BENCH_BYTES / 16;
            uint64_t t = run_job(job, BENCH_BYTES);
            if (!t)
            {
                log_printf("  %s: timeout\n", path ? "NDMA" : "CPU ");
                break;
            }
            size_t bad = 0;
            for (size_t w = 0; w < BENCH_BYTES / 4; w++)
            {
                u32 got = out_swap ? bswap(dst[w]) : dst[w];
                u32 want;
                memcpy(&want, ref + 4 * w, 4);
                bad += got != want;
            }
            if (bad == 0)
            {
                ok_swap = in_swap;
                best = t;
            }
            else if (in_swap)
                log_printf("  %s: %lu of %lu words wrong\n", path ? "NDMA" : "CPU ",
                           (unsigned long)bad, (unsigned long)(BENCH_BYTES / 4));
        }
        if (ok_swap >= 0)
            log_printf("  %s: %lu KB/s, verified (256 KB, input %s)\n",
                       path ? "NDMA" : "CPU ", (unsigned long)kbps(BENCH_BYTES, best),
                       ok_swap ? "byte-swapped" : "as is");
    }
    free(plain);

out:
    mbedtls_aes_free(&aes);
    free(job);
    free(src);
    free(dst);
    free(ref);
}

#else

void aes_bench(void)
{
}

#endif

void ghash_bench(void)
{
    u8 *buf = malloc(BENCH_BYTES_GHASH);
    if (!buf)
        return;
    memset(buf, 0x5A, BENCH_BYTES_GHASH);
    u8 h[16], out[16];
    for (int i = 0; i < 16; i++)
        h[i] = (u8)(i * 37 + 1);
    ghash_t g;
    ghash_init(&g, h);
    uint64_t t0 = prof_ticks();
    ghash_update(&g, buf, BENCH_BYTES_GHASH);
    uint64_t dt = prof_ticks() - t0;
    ghash_final(&g, out);
    free(buf);
    log_printf("CPU GHASH (8-bit table, ITCM): %lu KB/s\n",
               (unsigned long)(dt ? (uint64_t)BENCH_BYTES_GHASH * BUS_CLOCK / 1024 / dt : 0));
}

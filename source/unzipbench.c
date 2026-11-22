// SPDX-License-Identifier: MIT
//
// CPU speed test for zip unpacking (no SD or network I/O during the timed
// parts): compresses the first MBs of a ROM in RAM, then times inflate and
// CRC-32 variants on that data. See unzipbench.h.

#include "unzipbench.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "crc32fast.h"
#include "log.h"
#include "prof.h"
#include "unzip.h"

size_t zb_sys_deflate(const void *in, size_t n, void *out, size_t cap);
uint64_t zb_sys_inflate(const void *in, size_t n, uint8_t *out, size_t step,
                        uint32_t (*crcfn)(uint32_t, const void *, size_t), uint32_t *crc);
uint32_t zb_sys_crc32(uint32_t crc, const void *p, size_t n);

#define STEP (32 * 1024) // Output per inflate() call, as in unzip.c

static uint32_t kbps(size_t bytes, uint64_t ticks)
{
    return ticks ? (uint32_t)((uint64_t)bytes * BUS_CLOCK / 1024 / ticks) : 0;
}

static void row(const char *name, size_t bytes, uint64_t ticks, bool ok)
{
    log_printf("  %-34s %5lu ms %6lu KB/s%s\n", name,
               (unsigned long)(ticks * 1000 / BUS_CLOCK), (unsigned long)kbps(bytes, ticks),
               ok ? "" : "  WRONG OUTPUT");
}

void unzip_bench(const char *rom_path)
{
    log_printf("\n[Unzip CPU test] %s\n", rom_path);

    size_t n = 4 * 1024 * 1024;
    uint8_t *raw = NULL, *comp = NULL;
    while (n >= 512 * 1024)
    {
        raw = memalign(32, n);
        comp = memalign(32, n + n / 8 + 1024);
        if (raw && comp)
            break;
        free(raw);
        free(comp);
        raw = comp = NULL;
        n /= 2;
    }
    uint8_t *out = memalign(32, STEP);
    if (!raw || !out)
    {
        log_printf("  out of memory\n");
        goto done;
    }

    FILE *f = fopen(rom_path, "rb");
    size_t got = f ? fread(raw, 1, n, f) : 0;
    if (f)
        fclose(f);
    if (got < 64 * 1024)
    {
        log_printf("  can't read the ROM\n");
        goto done;
    }
    n = got;

    printf("Compressing %lu KB...\n", (unsigned long)(n / 1024));
    uint64_t t0 = prof_ticks();
    size_t cn = zb_sys_deflate(raw, n, comp, n + n / 8 + 1024);
    uint64_t dt = prof_ticks() - t0;
    if (!cn)
    {
        log_printf("  deflate failed\n");
        goto done;
    }
    log_printf("  data: %lu KB -> %lu KB (level 9, %lu ms)\n", (unsigned long)(n / 1024),
               (unsigned long)(cn / 1024), (unsigned long)(dt * 1000 / BUS_CLOCK));

    // Reference CRC
    uint32_t ref = zb_sys_crc32(0, raw, n);

    printf("CRC-32...\n");
    log_printf("  CRC-32 over %lu KB:\n", (unsigned long)(n / 1024));
    uint32_t c;
    t0 = prof_ticks();
    c = zb_sys_crc32(0, raw, n);
    row("zlib crc32 (toolchain)", n, prof_ticks() - t0, c == ref);
    t0 = prof_ticks();
    c = crc32_fast(0, raw, n);
    row("slicing-4, code+tables ITCM", n, prof_ticks() - t0, c == ref);

    printf("Inflate...\n");
    log_printf("  inflate to %lu KB (inflate() time only):\n", (unsigned long)(n / 1024));
    uint64_t tk = zb_sys_inflate(comp, cn, out, STEP, crc32_fast, &c);
    row("toolchain zlib (default alloc)", n, tk, tk && c == ref);

    // zip_bench_inflate returns microseconds
    uint64_t us = zip_bench_inflate(comp, cn, out, STEP, false, crc32_fast, &c);
    row("toolchain zlib, state RAM (alloc)", n, us * BUS_CLOCK / 1000000, us && c == ref);
    us = zip_bench_inflate(comp, cn, out, STEP, true, crc32_fast, &c);
    row(unzip_state_was_itcm ? "toolchain zlib, state ITCM"
                             : "toolchain zlib, state ITCM (DID NOT FIT)",
        n, us * BUS_CLOCK / 1000000, us && c == ref);

done:
    free(raw);
    free(comp);
    free(out);
}

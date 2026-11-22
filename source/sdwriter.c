// SPDX-License-Identifier: MIT
#include "sdwriter.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "prof.h"
#include "sdraw.h"

// Arbitrary, unique signal IDs for cothread_yield_signal().
#define SIG_DATA  0x53445744u // "SDWD": data available for the writer
#define SIG_SPACE 0x53445753u // "SDWS": space available for the producer
#define SIG_DONE  0x5344574Fu // "SDWO": writer thread finished

struct sdwriter
{
    FILE *fp;
    uint8_t *ring;
    size_t size;
    size_t chunk;
    volatile size_t head;  // Producer writes here
    volatile size_t tail;  // Writer reads here
    volatile size_t used;
    volatile bool closing;
    volatile bool finished;
    volatile bool failed;
    uint64_t write_ticks;
    uint64_t stall_ticks;

    // Raw mode: interrupt-driven DSpico DMA writes into the file's sectors
    const sdraw_extent_t *ext;
    int n_ext;
    uint32_t file_sector;  // Next sector of the file to write
    uint32_t sum;          // Checksum of everything written (sdw_checksum)
    volatile uint64_t written; // Bytes that reached the file
};

// Word-wise checksum of the data stream (speed test verification). Data
// arrives in pieces that are multiples of 4 bytes except at the very end.
static uint32_t checksum_add(uint32_t sum, const uint8_t *p, size_t n)
{
    size_t w = n / 4;
    const uint32_t *q = (const uint32_t *)p;
    for (size_t i = 0; i < w; i++)
        sum = sum * 31u + q[i];
    for (size_t i = w * 4; i < n; i++)
        sum = sum * 31u + p[i];
    return sum;
}

// Maps the next file sector to an LBA; *run = sectors left in that extent.
static uint32_t raw_lba(const sdwriter_t *w, uint32_t *run)
{
    uint32_t s = w->file_sector;
    for (int i = 0; i < w->n_ext; i++)
    {
        if (s < w->ext[i].count)
        {
            *run = w->ext[i].count - s;
            return w->ext[i].lba + s;
        }
        s -= w->ext[i].count;
    }
    *run = 0;
    return 0;
}

static bool raw_write(sdwriter_t *w, const uint8_t *src, size_t n)
{
    static uint32_t pad[128] __attribute__((aligned(32)));
    while (n > 0)
    {
        uint32_t run;
        uint32_t lba = raw_lba(w, &run);
        if (!lba)
            return false; // Past the allocated sectors
        const uint8_t *p = src;
        uint32_t sectors = n / 512;
        if (sectors == 0)
        {
            // Final partial sector: pad with zeros (beyond the file size)
            memset(pad, 0, sizeof(pad));
            memcpy(pad, src, n);
            p = (const uint8_t *)pad;
            sectors = 1;
        }
        if (sectors > run)
            sectors = run;

        static sdraw_op_t op;
        op.write = true;
        op.lba = lba;
        op.count = sectors;
        op.buf = (void *)p;
        if (!sdraw_op_submit(&op))
            return false;
        while (sdraw_op_pending(&op))
            cothread_yield(); // The network thread runs meanwhile
        if (op.state != SDRAW_OP_DONE)
            return false;

        size_t bytes = sectors * 512 < n ? sectors * 512 : n;
        w->file_sector += sectors;
        src += bytes;
        n -= bytes;
    }
    return true;
}

static int writer_thread(void *arg)
{
    sdwriter_t *w = arg;

    while (1)
    {
        size_t used = w->used;

        // Write full chunks; when closing, also flush the remainder.
        if (used == 0 || (used < w->chunk && !w->closing))
        {
            if (w->closing && used == 0)
                break;
            cothread_yield_signal(SIG_DATA);
            continue;
        }

        size_t n = used < w->chunk ? used : w->chunk;
        size_t until_end = w->size - w->tail;
        if (n > until_end)
            n = until_end;

        // Raw writes need whole sectors except for the final piece
        if (w->ext && n >= 512)
            n &= ~(size_t)511;

        if (!w->failed)
        {
            w->sum = checksum_add(w->sum, w->ring + w->tail, n);
            uint64_t t0 = prof_ticks();
            if (w->ext ? !raw_write(w, w->ring + w->tail, n)
                       : fwrite(w->ring + w->tail, 1, n, w->fp) != n)
                w->failed = true;
            else
                w->written += n;
            w->write_ticks += prof_ticks() - t0;
        }

        w->tail = (w->tail + n) % w->size;
        w->used -= n; // Single writer of this field per side: no lock needed
                      // because cothreads never preempt each other.
        cothread_send_signal(SIG_SPACE);

        // Give the network a chance between chunks.
        cothread_yield();
    }

    w->finished = true;
    cothread_send_signal(SIG_DONE);
    return 0;
}

sdwriter_t *sdw_open(FILE *fp, size_t ring_size, size_t chunk)
{
    sdwriter_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->ring = memalign(32, ring_size); // Cache lines (raw DMA)
    if (!w->ring)
    {
        free(w);
        return NULL;
    }
    w->fp = fp;
    w->size = ring_size;
    w->chunk = chunk;

    if (cothread_create(writer_thread, w, 4096, COTHREAD_DETACHED) == -1)
    {
        free(w->ring);
        free(w);
        return NULL;
    }
    return w;
}

sdwriter_t *sdw_open_raw(FILE *fp, size_t ring_size, size_t chunk,
                         const sdraw_extent_t *ext, int n_ext)
{
    if (!sdraw_async_init())
        return NULL;
    sdwriter_t *w = sdw_open(fp, ring_size, chunk);
    if (w)
    {
        w->ext = ext;
        w->n_ext = n_ext;
    }
    return w;
}

static uint64_t g_last_written;

uint64_t sdw_last_written(void)
{
    return g_last_written;
}

bool sdw_raw_seek(sdwriter_t *w, uint64_t offset)
{
    if (!w->ext || (offset & 511) != 0 || w->used != 0)
        return false;
    w->file_sector = (uint32_t)(offset / 512);
    return true;
}

uint64_t sdw_written(const sdwriter_t *w)
{
    return w->written;
}

uint32_t sdw_checksum(const sdwriter_t *w)
{
    return w->sum;
}

uint32_t sdw_checksum_data(uint32_t sum, const void *data, size_t n)
{
    return checksum_add(sum, data, n);
}

bool sdw_write(sdwriter_t *w, const void *data, size_t len)
{
    const uint8_t *src = data;

    while (len > 0)
    {
        if (w->failed)
            return false;

        size_t space = w->size - w->used;
        if (space == 0)
        {
            uint64_t t0 = prof_ticks();
            cothread_yield_signal(SIG_SPACE);
            w->stall_ticks += prof_ticks() - t0;
            continue;
        }

        size_t n = len < space ? len : space;
        size_t until_end = w->size - w->head;
        if (n > until_end)
            n = until_end;

        memcpy(w->ring + w->head, src, n);
        w->head = (w->head + n) % w->size;
        w->used += n;
        src += n;
        len -= n;

        if (w->used >= w->chunk)
            cothread_send_signal(SIG_DATA);
    }
    return true;
}

void *sdw_reserve(sdwriter_t *w, size_t *avail)
{
    while (!w->failed)
    {
        size_t space = w->size - w->used;
        if (space == 0)
        {
            uint64_t t0 = prof_ticks();
            cothread_yield_signal(SIG_SPACE);
            w->stall_ticks += prof_ticks() - t0;
            continue;
        }
        size_t until_end = w->size - w->head;
        *avail = space < until_end ? space : until_end;
        return w->ring + w->head;
    }
    *avail = 0;
    return NULL;
}

void sdw_commit(sdwriter_t *w, size_t n)
{
    w->head = (w->head + n) % w->size;
    w->used += n;
    if (w->used >= w->chunk)
        cothread_send_signal(SIG_DATA);
}

bool sdw_close_sum(sdwriter_t *w, uint32_t *sum)
{
    w->closing = true;
    while (!w->finished)
    {
        cothread_send_signal(SIG_DATA);
        cothread_yield_signal(SIG_DONE);
    }

    bool ok = !w->failed;
    if (sum)
        *sum = w->sum;
    g_last_written = w->written;
    free(w->ring);
    free(w);
    return ok;
}

bool sdw_close(sdwriter_t *w)
{
    return sdw_close_sum(w, NULL);
}

uint64_t sdw_write_ticks(const sdwriter_t *w)
{
    return w->write_ticks;
}

uint64_t sdw_stall_ticks(const sdwriter_t *w)
{
    return w->stall_ticks;
}

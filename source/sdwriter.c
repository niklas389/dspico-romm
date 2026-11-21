// SPDX-License-Identifier: MIT
#include "sdwriter.h"

#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "prof.h"

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
};

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

        if (!w->failed)
        {
            uint64_t t0 = prof_ticks();
            if (fwrite(w->ring + w->tail, 1, n, w->fp) != n)
                w->failed = true;
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
    w->ring = malloc(ring_size);
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

bool sdw_close(sdwriter_t *w)
{
    w->closing = true;
    while (!w->finished)
    {
        cothread_send_signal(SIG_DATA);
        cothread_yield_signal(SIG_DONE);
    }

    bool ok = !w->failed;
    free(w->ring);
    free(w);
    return ok;
}

uint64_t sdw_write_ticks(const sdwriter_t *w)
{
    return w->write_ticks;
}

uint64_t sdw_stall_ticks(const sdwriter_t *w)
{
    return w->stall_ticks;
}

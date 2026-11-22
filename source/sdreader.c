// SPDX-License-Identifier: MIT
//
// DSpico read-ahead (see sdreader.h).

#include "sdreader.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#define SLOTS      4
#define SLOT_BYTES (64 * 1024)

typedef struct
{
    sdraw_op_t op;
    uint8_t *buf;
    uint32_t start;   // Bytes of the slot before the wanted data (first slot)
    uint32_t len;     // Valid bytes in the slot
} slot_t;

struct sdreader
{
    const sdraw_extent_t *ext;
    int n_ext;
    uint32_t next_sector;   // File sector the next read starts at
    uint32_t end_sector;    // One past the last sector needed
    uint64_t end_byte;      // File offset after the last wanted byte
    uint64_t slot_byte[SLOTS]; // File offset of each slot's first byte
    slot_t slot[SLOTS];
    int cur;                // Slot being consumed
    uint32_t pos;           // Position in it
    int queued;             // Slots holding a read (pending or done)
    uint8_t *mem;
    bool failed;
};

// LBA of a file sector; *run = sectors left in that extent.
static uint32_t lba_of(const sdreader_t *r, uint32_t s, uint32_t *run)
{
    for (int i = 0; i < r->n_ext; i++)
    {
        if (s < r->ext[i].count)
        {
            *run = r->ext[i].count - s;
            return r->ext[i].lba + s;
        }
        s -= r->ext[i].count;
    }
    *run = 0;
    return 0;
}

// Queues a read into slot k for the next sectors. False at the end/on error.
static bool queue_slot(sdreader_t *r, int k)
{
    if (r->next_sector >= r->end_sector)
        return false;
    uint32_t run;
    uint32_t lba = lba_of(r, r->next_sector, &run);
    if (!lba)
    {
        r->failed = true;
        return false;
    }
    uint32_t count = SLOT_BYTES / 512;
    if (count > run)
        count = run;
    if (count > r->end_sector - r->next_sector)
        count = r->end_sector - r->next_sector;

    slot_t *sl = &r->slot[k];
    sl->op.write = false;
    sl->op.lba = lba;
    sl->op.count = count;
    sl->op.buf = sl->buf;
    r->slot_byte[k] = (uint64_t)r->next_sector * 512;
    uint64_t end = r->slot_byte[k] + count * 512;
    if (end > r->end_byte)
        end = r->end_byte;
    sl->start = 0;
    sl->len = (uint32_t)(end - r->slot_byte[k]);
    r->next_sector += count;
    if (!sdraw_op_submit(&sl->op))
    {
        r->failed = true;
        return false;
    }
    r->queued++;
    return true;
}

sdreader_t *sdr_open(const sdraw_extent_t *ext, int n_ext, uint64_t offset,
                     uint64_t len)
{
    if (!sdraw_async_init())
        return NULL;
    sdreader_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->mem = memalign(32, SLOTS * SLOT_BYTES);
    if (!r->mem)
    {
        free(r);
        return NULL;
    }
    r->ext = ext;
    r->n_ext = n_ext;
    r->next_sector = (uint32_t)(offset / 512);
    r->end_byte = offset + len;
    r->end_sector = (uint32_t)((r->end_byte + 511) / 512);
    for (int k = 0; k < SLOTS; k++)
        r->slot[k].buf = r->mem + k * SLOT_BYTES;

    for (int k = 0; k < SLOTS; k++)
        if (!queue_slot(r, k))
            break;
    if (r->queued)
        r->slot[0].start = (uint32_t)(offset - r->slot_byte[0]);
    r->pos = r->slot[0].start;
    return r;
}

int sdr_get(sdreader_t *r, const uint8_t **p, size_t *n)
{
    if (r->failed)
        return -1;
    if (r->queued == 0)
        return 0;
    slot_t *sl = &r->slot[r->cur];
    while (sdraw_op_pending(&sl->op))
        cothread_yield(); // The SD writer may queue its next write meanwhile
    if (sl->op.state != SDRAW_OP_DONE)
    {
        r->failed = true;
        return -1;
    }
    *p = sl->buf + r->pos;
    *n = sl->len - r->pos;
    return 1;
}

void sdr_consume(sdreader_t *r, size_t n)
{
    r->pos += n;
    slot_t *sl = &r->slot[r->cur];
    if (r->pos < sl->len)
        return;
    // Slot used up: refill it with the next sectors, move on
    r->queued--;
    int k = r->cur;
    queue_slot(r, k);
    r->cur = (r->cur + 1) % SLOTS;
    r->pos = r->slot[r->cur].start;
}

void sdr_close(sdreader_t *r)
{
    for (int k = 0; k < SLOTS; k++)
        while (r->slot[k].op.state && sdraw_op_pending(&r->slot[k].op))
            cothread_yield();
    free(r->mem);
    free(r);
}

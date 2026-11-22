// SPDX-License-Identifier: MIT
//
// Minimal zip reader for ROM archives (see unzip.h).

#include "unzip.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#ifdef __NDS__
#include <nds.h>
#include "prof.h"
static uint64_t now_us(void)
{
    return prof_ticks() * 1000000ull / BUS_CLOCK;
}
static void yield(void)
{
    cothread_yield(); // Lets the SD writer start its next DMA transfer
}
#else
#include <time.h>
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}
static void yield(void)
{
}
#endif

#define IN_BUF    (64 * 1024)
#define OUT_STEP  (32 * 1024)   // Output per inflate() call
#define EOCD_SCAN (65535 + 22)  // Max comment + EOCD record

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static bool read_at(FILE *f, uint64_t off, void *buf, size_t n)
{
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

bool zip_is_archive_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot + 1, "zip") == 0;
}

static bool ext_in_list(const char *name, const char *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot || strchr(dot, '/'))
        return false;
    dot++;
    size_t n = strlen(dot);
    for (const char *p = exts; *p;)
    {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncasecmp(p, dot, n) == 0)
            return true;
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}

// Locates the central directory. Returns false if this is no zip file.
static bool find_cd(const zip_reader_t *r, uint64_t *cd_off, uint64_t *cd_size,
                    uint64_t *count)
{
    if (r->size < 22)
        return false;

    size_t scan = r->size < EOCD_SCAN ? (size_t)r->size : EOCD_SCAN;
    uint8_t *buf = malloc(scan);
    if (!buf)
        return false;
    uint64_t base = r->size - scan;
    bool ok = false;
    if (r->read(r->ctx, base, buf, scan))
    {
        for (size_t i = scan - 22 + 1; i-- > 0;)
        {
            if (rd32(buf + i) != 0x06054b50)
                continue;
            *count = rd16(buf + i + 10);
            *cd_size = rd32(buf + i + 12);
            *cd_off = rd32(buf + i + 16);
            ok = true;

            // ZIP64: the locator sits right before the EOCD record
            if (i >= 20 && rd32(buf + i - 20) == 0x07064b50)
            {
                uint8_t z[56];
                if (r->read(r->ctx, rd64(buf + i - 20 + 8), z, sizeof(z)) &&
                    rd32(z) == 0x06064b50)
                {
                    *count = rd64(z + 32);
                    *cd_size = rd64(z + 40);
                    *cd_off = rd64(z + 48);
                }
                else
                    ok = false;
            }
            break;
        }
    }
    free(buf);
    return ok;
}

static bool file_read(void *ctx, uint64_t off, void *buf, size_t n)
{
    return read_at(ctx, off, buf, n);
}

bool zip_find_rom(FILE *f, const char *exts, zip_entry_t *e, char *err, int err_len)
{
    zip_reader_t r = { file_read, f, 0 };
    if (fseeko(f, 0, SEEK_END) != 0)
    {
        snprintf(err, err_len, "can't read the archive");
        return false;
    }
    r.size = (uint64_t)ftello(f);
    return zip_find_rom_r(&r, exts, e, err, err_len);
}

bool zip_find_rom_r(const zip_reader_t *r, const char *exts, zip_entry_t *best,
                    char *err, int err_len)
{
    memset(best, 0, sizeof(*best));
    uint64_t cd_off, cd_size, count;
    if (!find_cd(r, &cd_off, &cd_size, &count))
    {
        snprintf(err, err_len, "not a zip file");
        return false;
    }
    if (cd_size > 4 * 1024 * 1024)
    {
        snprintf(err, err_len, "zip directory too large");
        return false;
    }
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    if (!cd || !r->read(r->ctx, cd_off, cd, cd_size))
    {
        free(cd);
        snprintf(err, err_len, "can't read the zip directory");
        return false;
    }

    bool found = false;
    char first[64] = "";
    size_t p = 0;
    int entries = 0;
    for (uint64_t i = 0; i < count && p + 46 <= cd_size; i++)
    {
        const uint8_t *h = cd + p;
        if (rd32(h) != 0x02014b50)
            break;
        uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (p + 46 + nlen + xlen > cd_size)
            break;

        zip_entry_t e = { 0 };
        e.flags = rd16(h + 8);
        e.method = rd16(h + 10);
        e.crc = rd32(h + 16);
        e.comp_size = rd32(h + 20);
        e.size = rd32(h + 24);
        e.local_offset = rd32(h + 42);
        size_t n = nlen < sizeof(e.name) - 1 ? nlen : sizeof(e.name) - 1;
        memcpy(e.name, h + 46, n);

        // ZIP64 extra field: the 64-bit values of fields set to 0xFFFFFFFF
        const uint8_t *x = h + 46 + nlen, *xend = x + xlen;
        while (x + 4 <= xend)
        {
            uint16_t id = rd16(x), len = rd16(x + 2);
            const uint8_t *v = x + 4, *vend = v + len;
            if (vend > xend)
                break;
            if (id == 0x0001)
            {
                if (e.size == 0xFFFFFFFF && v + 8 <= vend)
                    e.size = rd64(v), v += 8;
                if (e.comp_size == 0xFFFFFFFF && v + 8 <= vend)
                    e.comp_size = rd64(v), v += 8;
                if (e.local_offset == 0xFFFFFFFF && v + 8 <= vend)
                    e.local_offset = rd64(v), v += 8;
            }
            x = vend;
        }
        p += 46 + nlen + xlen + clen;

        if (n == 0 || e.name[n - 1] == '/')
            continue; // Directory
        entries++;
        if (!first[0])
            snprintf(first, sizeof(first), "%.63s", e.name);
        if (ext_in_list(e.name, exts) && (!found || e.size > best->size))
        {
            *best = e;
            found = true;
        }
    }
    free(cd);

    if (!found)
    {
        snprintf(err, err_len, "no .%s in the archive (%d files, e.g. %.40s)",
                 exts, entries, first);
        return false;
    }
    best->entries = entries;
    if (best->flags & 1)
    {
        snprintf(err, err_len, "encrypted zip entries are not supported");
        return false;
    }
    if (best->method != 0 && best->method != 8)
    {
        snprintf(err, err_len, "zip method %u not supported (only deflate)",
                 best->method);
        return false;
    }
    return true;
}

bool zip_extract(FILE *f, const zip_entry_t *e, const zip_sink_t *sink,
                 zip_progress_fn cb, void *ctx, zip_stats_t *st)
{
    memset(st, 0, sizeof(*st));

    uint8_t lh[30];
    if (!read_at(f, e->local_offset, lh, sizeof(lh)) || rd32(lh) != 0x04034b50)
    {
        snprintf(st->error, sizeof(st->error), "bad local header");
        return false;
    }
    uint64_t data = e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (fseeko(f, (off_t)data, SEEK_SET) != 0)
    {
        snprintf(st->error, sizeof(st->error), "seek failed");
        return false;
    }

    uint8_t *in = NULL;
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (e->method == 8)
    {
        in = malloc(IN_BUF);
        if (!in || inflateInit2(&z, -MAX_WBITS) != Z_OK)
        {
            free(in);
            snprintf(st->error, sizeof(st->error), "out of memory");
            return false;
        }
    }

    uint64_t in_left = e->comp_size, out_left = e->size;
    uint32_t crc = crc32(0, NULL, 0);
    bool ok = true, ended = false;

    while (ok && out_left > 0)
    {
        uint64_t t0 = now_us();
        size_t avail;
        uint8_t *out = sink->reserve(sink->ctx, &avail);
        st->sink_us += now_us() - t0;
        if (!out)
        {
            snprintf(st->error, sizeof(st->error), "SD write failed");
            ok = false;
            break;
        }
        if (avail > OUT_STEP)
            avail = OUT_STEP;
        if (avail > out_left)
            avail = (size_t)out_left;

        size_t produced;
        if (e->method == 0)
        {
            t0 = now_us();
            produced = fread(out, 1, avail, f);
            st->read_us += now_us() - t0;
            st->in_bytes += produced;
            if (produced != avail)
            {
                snprintf(st->error, sizeof(st->error), "archive truncated");
                ok = false;
            }
        }
        else
        {
            if (z.avail_in == 0 && in_left > 0)
            {
                size_t n = in_left < IN_BUF ? (size_t)in_left : IN_BUF;
                t0 = now_us();
                size_t got = fread(in, 1, n, f);
                st->read_us += now_us() - t0;
                if (got != n)
                {
                    snprintf(st->error, sizeof(st->error), "archive read failed");
                    ok = false;
                    break;
                }
                in_left -= n;
                st->in_bytes += n;
                z.next_in = in;
                z.avail_in = (uInt)n;
            }
            z.next_out = out;
            z.avail_out = (uInt)avail;
            t0 = now_us();
            int ret = inflate(&z, Z_NO_FLUSH);
            st->inflate_us += now_us() - t0;
            produced = avail - z.avail_out;
            if (ret == Z_STREAM_END)
                ended = true;
            else if (ret != Z_OK && !(ret == Z_BUF_ERROR && produced))
            {
                snprintf(st->error, sizeof(st->error), "inflate error %d%s%s", ret,
                         z.msg ? ": " : "", z.msg ? z.msg : "");
                ok = false;
            }
            else if (produced == 0 && z.avail_in == 0 && in_left == 0)
            {
                snprintf(st->error, sizeof(st->error), "compressed data truncated");
                ok = false;
            }
        }

        t0 = now_us();
        crc = crc32(crc, out, (uInt)produced);
        st->crc_us += now_us() - t0;
        sink->commit(sink->ctx, produced);
        out_left -= produced;
        st->out_bytes += produced;
        yield();

        if (ended && out_left > 0 && ok)
        {
            snprintf(st->error, sizeof(st->error), "data ends early");
            ok = false;
        }
        if (ok && cb && !cb(ctx, st->out_bytes, e->size))
        {
            st->cancelled = true;
            snprintf(st->error, sizeof(st->error), "cancelled");
            ok = false;
        }
    }

    if (ok && e->method == 8 && !ended)
    {
        // All output produced: the stream must end here, without more data
        uint8_t extra;
        z.next_out = &extra;
        z.avail_out = 1;
        int ret = Z_OK;
        while (ret == Z_OK && z.avail_out == 1)
        {
            if (z.avail_in == 0 && in_left > 0)
            {
                size_t n = in_left < IN_BUF ? (size_t)in_left : IN_BUF;
                if (fread(in, 1, n, f) != n)
                    break;
                in_left -= n;
                z.next_in = in;
                z.avail_in = (uInt)n;
            }
            ret = inflate(&z, Z_NO_FLUSH);
            if (ret == Z_BUF_ERROR && z.avail_in == 0 && in_left == 0)
                break;
        }
        if (ret != Z_STREAM_END || z.avail_out != 1)
        {
            snprintf(st->error, sizeof(st->error), "size mismatch");
            ok = false;
        }
    }
    if (ok && crc != e->crc)
    {
        snprintf(st->error, sizeof(st->error), "CRC mismatch (%08lx, expected %08lx)",
                 (unsigned long)crc, (unsigned long)e->crc);
        ok = false;
    }

    if (e->method == 8)
        inflateEnd(&z);
    free(in);
    return ok;
}

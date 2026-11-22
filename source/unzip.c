// SPDX-License-Identifier: MIT
//
// Minimal zip reader for ROM archives (see unzip.h).

#include "unzip.h"
#include "crc32fast.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zlib.h> // Toolchain zlib 1.4 (faster than 1.3.1 here, see unzipbench)

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

// The inflate state (~7 KB, mostly the decoding tables) lives in ITCM: no
// data cache misses on table lookups. The 32 KB window stays in main RAM.
#ifdef __NDS__
#define STATE_MEM ITCM_BSS
#else
#define STATE_MEM
#endif
static uint8_t itcm_state[8192] STATE_MEM __attribute__((aligned(8)));
bool unzip_state_was_itcm; // Last inflateInit2 got the ITCM buffer
static bool itcm_state_used;
bool unzip_state_itcm = true;

voidpf unzip_zalloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    size_t n = (size_t)items * size;
    if (unzip_state_itcm && !itcm_state_used && n <= sizeof(itcm_state))
    {
        itcm_state_used = true;
        unzip_state_was_itcm = true;
        return itcm_state;
    }
    return malloc(n);
}

void unzip_zfree(voidpf opaque, voidpf p)
{
    (void)opaque;
    if (p == itcm_state)
        itcm_state_used = false;
    else
        free(p);
}

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

bool zip_data_offset(FILE *f, const zip_entry_t *e, uint64_t *off)
{
    uint8_t lh[30];
    if (!read_at(f, e->local_offset, lh, sizeof(lh)) || rd32(lh) != 0x04034b50)
        return false;
    *off = e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    return true;
}

// --- FILE source (fread into a buffer) ---------------------------------------

typedef struct
{
    FILE *f;
    uint8_t *buf;
    size_t pos, len;
    uint64_t left;
} file_src_t;

static int file_get(void *ctx, const uint8_t **p, size_t *n)
{
    file_src_t *fs = ctx;
    if (fs->pos == fs->len)
    {
        if (fs->left == 0)
            return 0;
        size_t want = fs->left < IN_BUF ? (size_t)fs->left : IN_BUF;
        if (fread(fs->buf, 1, want, fs->f) != want)
            return -1;
        fs->left -= want;
        fs->pos = 0;
        fs->len = want;
    }
    *p = fs->buf + fs->pos;
    *n = fs->len - fs->pos;
    return 1;
}

static void file_consume(void *ctx, size_t n)
{
    ((file_src_t *)ctx)->pos += n;
}

bool zip_extract(FILE *f, const zip_entry_t *e, const zip_sink_t *sink,
                 zip_progress_fn cb, void *ctx, zip_stats_t *st)
{
    uint64_t data;
    if (!zip_data_offset(f, e, &data) || fseeko(f, (off_t)data, SEEK_SET) != 0)
    {
        memset(st, 0, sizeof(*st));
        snprintf(st->error, sizeof(st->error), "bad local header");
        return false;
    }
    file_src_t fs = { f, malloc(IN_BUF), 0, 0, e->comp_size };
    if (!fs.buf)
    {
        memset(st, 0, sizeof(*st));
        snprintf(st->error, sizeof(st->error), "out of memory");
        return false;
    }
    zip_source_t src = { file_get, file_consume, &fs };
    bool ok = zip_extract_src(&src, e, sink, cb, ctx, st);
    free(fs.buf);
    return ok;
}

// --- Extraction ---------------------------------------------------------------

typedef struct
{
    const zip_source_t *src;
    zip_stats_t *st;
    size_t given;        // Bytes handed to the consumer from the current piece
} in_t;

// Next piece of input; 1 data, 0 end, -1 error. Releases the previous piece.
static int next_input(in_t *in, const uint8_t **p, size_t *n)
{
    if (in->given)
    {
        in->src->consume(in->src->ctx, in->given);
        in->given = 0;
    }
    uint64_t t0 = now_us();
    int r = in->src->get(in->src->ctx, p, n);
    in->st->read_us += now_us() - t0;
    if (r > 0)
    {
        in->given = *n;
        in->st->in_bytes += *n;
    }
    return r;
}

bool zip_extract_src(const zip_source_t *src, const zip_entry_t *e,
                     const zip_sink_t *sink, zip_progress_fn cb, void *ctx,
                     zip_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    in_t in = { src, st, 0 };

    z_stream z;
    memset(&z, 0, sizeof(z));
    if (e->method == 8)
    {
        z.zalloc = unzip_zalloc;
        z.zfree = unzip_zfree;
        unzip_state_was_itcm = false;
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        {
            snprintf(st->error, sizeof(st->error), "out of memory");
            return false;
        }
    }

    uint64_t out_left = e->size;
    uint32_t crc = 0;
    bool ok = true, ended = false, in_end = false;
    const uint8_t *sp = NULL; // Stored: current input piece
    size_t sn = 0;

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

        size_t produced = 0;
        if (e->method == 0)
        {
            while (ok && produced < avail)
            {
                if (sn == 0)
                {
                    int r = next_input(&in, &sp, &sn);
                    if (r <= 0)
                    {
                        snprintf(st->error, sizeof(st->error),
                                 r < 0 ? "archive read failed" : "archive truncated");
                        ok = false;
                        break;
                    }
                }
                size_t k = sn < avail - produced ? sn : avail - produced;
                t0 = now_us();
                memcpy(out + produced, sp, k);
                st->inflate_us += now_us() - t0;
                sp += k;
                sn -= k;
                produced += k;
            }
        }
        else
        {
            if (z.avail_in == 0 && !in_end)
            {
                const uint8_t *p;
                size_t n;
                int r = next_input(&in, &p, &n);
                if (r < 0)
                {
                    snprintf(st->error, sizeof(st->error), "archive read failed");
                    ok = false;
                    break;
                }
                if (r == 0)
                    in_end = true;
                else
                {
                    z.next_in = (Bytef *)p;
                    z.avail_in = (uInt)n;
                }
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
                if (ret == Z_BUF_ERROR && in_end)
                    snprintf(st->error, sizeof(st->error), "compressed data truncated");
                else
                    snprintf(st->error, sizeof(st->error), "inflate error %d%s%s", ret,
                             z.msg ? ": " : "", z.msg ? z.msg : "");
                ok = false;
            }
        }

        t0 = now_us();
        crc = crc32_fast(crc, out, produced);
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
            if (z.avail_in == 0 && !in_end)
            {
                const uint8_t *p;
                size_t n;
                int r = next_input(&in, &p, &n);
                if (r <= 0)
                {
                    in_end = true;
                    if (r < 0)
                        break;
                }
                else
                {
                    z.next_in = (Bytef *)p;
                    z.avail_in = (uInt)n;
                }
            }
            ret = inflate(&z, Z_NO_FLUSH);
            if (ret == Z_BUF_ERROR && z.avail_in == 0 && in_end)
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
    return ok;
}

// --- Speed test --------------------------------------------------------------

uint64_t zip_bench_inflate(const void *in, size_t n, uint8_t *out, size_t step,
                           bool state_in_itcm,
                           uint32_t (*crcfn)(uint32_t, const void *, size_t),
                           uint32_t *crc)
{
    unzip_state_itcm = state_in_itcm;
    unzip_state_was_itcm = false;

    z_stream z;
    memset(&z, 0, sizeof(z));
    z.zalloc = unzip_zalloc;
    z.zfree = unzip_zfree;
    uint64_t ticks = 0;
    uint32_t c = 0;
    int r = inflateInit2(&z, -MAX_WBITS);
    if (r == Z_OK)
    {
        z.next_in = (Bytef *)in;
        z.avail_in = (uInt)n;
        while (r == Z_OK)
        {
            z.next_out = out;
            z.avail_out = (uInt)step;
            uint64_t t0 = now_us();
            r = inflate(&z, Z_NO_FLUSH);
            ticks += now_us() - t0;
            c = crcfn(c, out, step - z.avail_out);
        }
        inflateEnd(&z);
    }
    unzip_state_itcm = true;
    *crc = c;
    return r == Z_STREAM_END ? (ticks ? ticks : 1) : 0;
}

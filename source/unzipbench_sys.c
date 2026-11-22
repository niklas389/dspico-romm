// SPDX-License-Identifier: MIT
//
// Speed test helpers using the toolchain's zlib (the one libcurl links), in
// a file of their own because its zlib.h can't be mixed with the vendored
// copy (third_party/zlib, z_ prefix).

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

#include "prof.h"

// Raw deflate (zip method 8), level 9. Returns the compressed size, 0 on error.
size_t zb_sys_deflate(const void *in, size_t n, void *out, size_t cap)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    int r = deflate(&z, Z_FINISH);
    size_t got = cap - z.avail_out;
    deflateEnd(&z);
    return r == Z_STREAM_END ? got : 0;
}

// Inflates in steps of `step` bytes into out (reused); returns the inflate()
// time in BUS_CLOCK ticks, 0 on error. *crc gets the output's CRC-32.
uint64_t zb_sys_inflate(const void *in, size_t n, uint8_t *out, size_t step,
                        uint32_t (*crcfn)(uint32_t, const void *, size_t), uint32_t *crc)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        return 0;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    uint64_t ticks = 0;
    uint32_t c = 0;
    int r = Z_OK;
    while (r == Z_OK)
    {
        z.next_out = out;
        z.avail_out = (uInt)step;
        uint64_t t0 = prof_ticks();
        r = inflate(&z, Z_NO_FLUSH);
        ticks += prof_ticks() - t0;
        c = crcfn(c, out, step - z.avail_out);
    }
    inflateEnd(&z);
    *crc = c;
    return r == Z_STREAM_END ? (ticks ? ticks : 1) : 0;
}

uint32_t zb_sys_crc32(uint32_t crc, const void *p, size_t n)
{
    return crc32(crc, p, (uInt)n);
}

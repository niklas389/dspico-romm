// SPDX-License-Identifier: MIT
// Host test for source/unzip.c: extracts the ROM of a zip to a file.
//   cc -O2 -Isource tools/hosttest/unzip_host.c source/unzip.c -lz -o unzip_host
//   ./unzip_host in.zip exts out.bin [sink_chunk]
#include <stdio.h>
#include <stdlib.h>
#include "unzip.h"

typedef struct { FILE *f; unsigned char buf[100000]; size_t chunk; } sink_t;

static void *res(void *c, size_t *avail)
{
    sink_t *s = c;
    *avail = s->chunk; // Odd sizes exercise the partial-output paths
    return s->buf;
}
static void com(void *c, size_t n)
{
    sink_t *s = c;
    fwrite(s->buf, 1, n, s->f);
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    zip_entry_t e;
    char err[128];
    if (!zip_find_rom(f, argv[2], &e, err, sizeof(err)))
    {
        printf("find: %s\n", err);
        return 1;
    }
    printf("entry %s method %u size %llu comp %llu files %d\n", e.name, e.method,
           (unsigned long long)e.size, (unsigned long long)e.comp_size, e.entries);
    static sink_t s;
    s.f = fopen(argv[3], "wb");
    s.chunk = argc > 4 ? (size_t)atoi(argv[4]) : sizeof(s.buf);
    zip_sink_t sink = { res, com, &s };
    zip_stats_t st;
    bool ok = zip_extract(f, &e, &sink, NULL, NULL, &st);
    fclose(s.f);
    printf("%s: in %llu out %llu inflate %llu us crc %llu us %s\n", ok ? "ok" : "FAIL",
           (unsigned long long)st.in_bytes, (unsigned long long)st.out_bytes,
           (unsigned long long)st.inflate_us, (unsigned long long)st.crc_us, st.error);
    return ok ? 0 : 1;
}

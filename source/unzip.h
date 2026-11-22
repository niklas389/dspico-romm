// SPDX-License-Identifier: MIT
#ifndef ROMM_UNZIP_H
#define ROMM_UNZIP_H

// Minimal zip reader for ROM archives: finds the ROM inside a .zip (stored
// or deflate, ZIP64 aware) and streams it, inflated, into a sink. The CRC-32
// of the output is checked against the archive.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct
{
    char name[256];          // Path inside the archive
    uint64_t comp_size;
    uint64_t size;           // Uncompressed
    uint64_t local_offset;   // Local file header
    uint32_t crc;
    uint16_t method;         // 0 = stored, 8 = deflate
    uint16_t flags;
    int entries;             // Files in the archive (for the log)
} zip_entry_t;

// Output: the extractor asks for space, fills it, commits it (zero copy into
// the SD writer's ring). reserve returns NULL on a write error.
typedef struct
{
    void *(*reserve)(void *ctx, size_t *avail);
    void (*commit)(void *ctx, size_t n);
    void *ctx;
} zip_sink_t;

// Progress: return false to cancel.
typedef bool (*zip_progress_fn)(void *ctx, uint64_t done, uint64_t total);

typedef struct
{
    uint64_t in_bytes, out_bytes;
    uint64_t read_us;        // Waiting for archive data
    uint64_t inflate_us;     // inflate() or copy
    uint64_t crc_us;
    uint64_t sink_us;        // Waiting for space in the sink
    bool cancelled;
    char error[128];
} zip_stats_t;

// Picks the ROM: the largest file whose extension is in exts (comma-
// separated, e.g. "nds,dsi,srl"), else fails. False with err set on errors.
bool zip_find_rom(FILE *f, const char *exts, zip_entry_t *e, char *err, int err_len);

// Same, reading through a callback (e.g. HTTP Range requests, to learn the
// ROM size before downloading). size: archive size.
typedef struct
{
    bool (*read)(void *ctx, uint64_t off, void *buf, size_t n);
    void *ctx;
    uint64_t size;
} zip_reader_t;
bool zip_find_rom_r(const zip_reader_t *r, const char *exts, zip_entry_t *e,
                    char *err, int err_len);

bool zip_extract(FILE *f, const zip_entry_t *e, const zip_sink_t *sink,
                 zip_progress_fn cb, void *ctx, zip_stats_t *st);

// Input for zip_extract_src: the entry's compressed bytes in order. get
// returns the next contiguous piece (1), 0 at the end, -1 on errors; the
// piece stays valid until consume(n) is called with its size.
typedef struct
{
    int (*get)(void *ctx, const uint8_t **p, size_t *n);
    void (*consume)(void *ctx, size_t n);
    void *ctx;
} zip_source_t;

// File offset of the entry's data (after the local header).
bool zip_data_offset(FILE *f, const zip_entry_t *e, uint64_t *off);

bool zip_extract_src(const zip_source_t *src, const zip_entry_t *e,
                     const zip_sink_t *sink, zip_progress_fn cb, void *ctx,
                     zip_stats_t *st);

// Speed test knob: inflate state in ITCM (default) or main RAM.
extern bool unzip_state_itcm;
extern bool unzip_state_was_itcm; // The last inflate state fit into ITCM

// Speed test: inflates a raw deflate stream in steps; returns microseconds
// spent in inflate(), 0 on error.
uint64_t zip_bench_inflate(const void *in, size_t n, uint8_t *out, size_t step,
                           bool state_in_itcm,
                           uint32_t (*crcfn)(uint32_t, const void *, size_t),
                           uint32_t *crc);

// True for file names the library stores as archives (.zip).
bool zip_is_archive_name(const char *name);

#endif

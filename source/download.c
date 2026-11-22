// SPDX-License-Identifier: MIT
//
// ROM downloads into the SD card library (see download.h).

#include "download.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dl.h"
#include "log.h"
#include "sdraw.h"
#include "sdwriter.h"
#include "unzip.h"

#define DL_ATTEMPTS  5
#define RING_SIZE    (1024 * 1024)
#define WRITE_CHUNK  (32 * 1024)

const char *download_folder(const char *slug, const char *fs_slug)
{
    static const struct { const char *slug, *folder; } map[] = {
        { "nds", "nds" },
        { "nintendo-ds", "nds" },
        { "nintendo-dsi", "dsi" },
        { "dsi", "dsi" },
        { "gba", "gba" },
        { "game-boy-advance", "gba" },
        { "gb", "gb" },
        { "gbc", "gbc" },
    };
    if (slug)
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (strcmp(slug, map[i].slug) == 0)
                return map[i].folder;
    if (fs_slug && fs_slug[0])
        return fs_slug;
    return slug && slug[0] ? slug : "other";
}

// "fat:/romm/" -> "fat:/"
static void drive_of(const config_t *cfg, char *out, int out_len)
{
    const char *colon = strstr(cfg->base_dir, ":/");
    int n = colon ? (int)(colon - cfg->base_dir) + 2 : 0;
    if (n <= 0 || n >= out_len)
        snprintf(out, out_len, "fat:/");
    else
        snprintf(out, out_len, "%.*s", n, cfg->base_dir);
}

void download_target(const config_t *cfg, const char *folder, const char *fs_name,
                     char *out, int out_len)
{
    char drive[16];
    drive_of(cfg, drive, sizeof(drive));
    snprintf(out, out_len, "%sroms/%s/%s", drive, folder, fs_name);
}

bool download_exists(const config_t *cfg, const char *folder, const char *fs_name,
                     uint64_t *size)
{
    char path[CFG_PATH_MAX + 256];
    download_target(cfg, folder, fs_name, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    if (size)
        *size = (uint64_t)st.st_size;
    return true;
}

static void make_dirs(const config_t *cfg, const char *folder)
{
    char drive[16], dir[CFG_PATH_MAX + 64];
    drive_of(cfg, drive, sizeof(drive));
    snprintf(dir, sizeof(dir), "%sroms", drive);
    mkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%sroms/%s", drive, folder);
    mkdir(dir, 0777);
}

// Reserve the whole file without writing data (DLDI path).
static bool preallocate(FILE *f, uint64_t size)
{
    if (size == 0)
        return true;
    return fseek(f, (long)size - 1, SEEK_SET) == 0 && fputc(0, f) != EOF &&
           fflush(f) == 0 && fseek(f, 0, SEEK_SET) == 0;
}

// Creates path and allocates size bytes. With the DSpico writer enabled, also
// returns the file's sectors (*raw = true); otherwise the file is preallocated
// for DLDI writes. NULL with err set on failure.
static FILE *open_output(const config_t *cfg, const char *path, uint64_t size,
                         sdraw_extent_t *ext, int max_ext, int *n_ext, bool *raw,
                         char *err, int err_len)
{
    FILE *f = fopen(path, "w+b");
    if (!f)
    {
        snprintf(err, err_len, "can't create %.140s", path);
        return NULL;
    }
    setvbuf(f, NULL, _IONBF, 0); // The SD writer writes in large chunks

    *n_ext = -1;
    *raw = cfg->sdraw && sdraw_is_dspico();
    if (*raw)
    {
        *n_ext = sdraw_map(f, (uint32_t)size, ext, max_ext);
        if (*n_ext <= 0)
        {
            log_printf("  raw SD writes unavailable (%d extents), using DLDI\n", *n_ext);
            *raw = false;
        }
    }
    if (!*raw && !preallocate(f, size))
    {
        fclose(f);
        remove(path);
        snprintf(err, err_len, "not enough space on the SD card");
        return NULL;
    }
    return f;
}

bool download_rom(const config_t *cfg, int rom_id, const char *fs_name, uint64_t size,
                  const char *folder, net_progress_fn cb, void *ctx,
                  download_result_t *res)
{
    memset(res, 0, sizeof(*res));
    uint32_t t0 = net_now_ms();

    if (size == 0 || size > 0xFFFFFFFFull)
    {
        snprintf(res->error, sizeof(res->error), "unsupported size");
        return false;
    }

    char *escaped = net_escape(fs_name);
    if (!escaped)
    {
        snprintf(res->error, sizeof(res->error), "out of memory");
        return false;
    }
    char url_path[768];
    snprintf(url_path, sizeof(url_path), "/api/roms/%d/content/%s", rom_id, escaped);
    free(escaped);

    make_dirs(cfg, folder);
    download_target(cfg, folder, fs_name, res->path, sizeof(res->path));
    char part[sizeof(res->path) + 8];
    snprintf(part, sizeof(part), "%s.part", res->path);

    static sdraw_extent_t ext[64];
    int n_ext = 0;
    FILE *f = open_output(cfg, part, size, ext, 64, &n_ext, &res->raw,
                          res->error, sizeof(res->error));
    if (!f)
        return false;

    uint64_t offset = 0;
    while (offset < size && res->attempts < DL_ATTEMPTS)
    {
        res->attempts++;
        sdwriter_t *w = res->raw
                      ? sdw_open_raw(f, RING_SIZE, WRITE_CHUNK, ext, n_ext)
                      : (fseek(f, (long)offset, SEEK_SET) == 0
                             ? sdw_open(f, RING_SIZE, WRITE_CHUNK) : NULL);
        if (w && res->raw)
            sdw_raw_seek(w, offset);
        if (!w)
        {
            snprintf(res->error, sizeof(res->error), "SD writer init failed");
            break;
        }

        net_result_t r;
        bool ok = dl_get_range(url_path, offset, size - 1, w, cb, ctx, &r);
        bool wok = sdw_close(w);
        uint64_t written = sdw_last_written();
        offset += written;

        if (r.curl_code == 42) // Cancelled (B)
        {
            res->cancelled = true;
            snprintf(res->error, sizeof(res->error), "cancelled");
            break;
        }
        if (!wok)
        {
            snprintf(res->error, sizeof(res->error), "SD write failed");
            break;
        }
        if (ok && r.status != 206 && !(r.status == 200 && offset == written))
        {
            snprintf(res->error, sizeof(res->error), "HTTP %ld", r.status);
            break;
        }
        if (!ok)
        {
            snprintf(res->error, sizeof(res->error), "%.150s", r.error);
            log_printf("  attempt %d stopped at %llu B: %s\n", res->attempts,
                       (unsigned long long)offset, r.error);
            if (r.status == 401 || r.status == 403 || r.status == 404)
                break;
            // Raw writes need whole sectors: resume at a sector boundary
            // (the partial sector is downloaded again).
            if (res->raw)
                offset &= ~(uint64_t)511;
            net_wifi_ensure();
        }
    }

    res->bytes = offset < size ? offset : size;
    fclose(f);

    if (offset >= size)
    {
        remove(res->path); // Replace an older copy
        if (rename(part, res->path) == 0)
            res->ok = true;
        else
            snprintf(res->error, sizeof(res->error), "rename failed (%d)", errno);
    }
    if (!res->ok)
        remove(part);

    res->ms = net_now_ms() - t0;
    return res->ok;
}

// --- Archives ---------------------------------------------------------------

// ROM extensions looked for inside archives, by library folder.
static const char *rom_exts(const char *folder)
{
    if (strcmp(folder, "nds") == 0 || strcmp(folder, "dsi") == 0)
        return "nds,dsi,srl";
    if (strcmp(folder, "gba") == 0)
        return "gba";
    if (strcmp(folder, "gb") == 0 || strcmp(folder, "gbc") == 0)
        return "gbc,gb";
    return "nds,dsi,srl,gba,gbc,gb";
}

bool download_is_archive(const char *fs_name)
{
    return zip_is_archive_name(fs_name);
}

bool download_local_name(const char *folder, const char *fs_name, int i,
                         char *out, int out_len)
{
    if (!zip_is_archive_name(fs_name))
    {
        if (i != 0)
            return false;
        snprintf(out, out_len, "%s", fs_name);
        return true;
    }
    // "<stem>.<ext>" for the i-th extension of the folder's list
    const char *exts = rom_exts(folder);
    for (int k = 0; k < i; k++)
    {
        exts = strchr(exts, ',');
        if (!exts)
            return false;
        exts++;
    }
    const char *end = strchr(exts, ',');
    int elen = end ? (int)(end - exts) : (int)strlen(exts);
    int slen = (int)(strrchr(fs_name, '.') - fs_name);
    snprintf(out, out_len, "%.*s.%.*s", slen, fs_name, elen, exts);
    return true;
}

bool download_local(const config_t *cfg, const char *folder, const char *fs_name,
                    char *name, int name_len, uint64_t *size)
{
    char cand[256];
    for (int i = 0; download_local_name(folder, fs_name, i, cand, sizeof(cand)); i++)
    {
        if (download_exists(cfg, folder, cand, size))
        {
            if (name)
                snprintf(name, name_len, "%s", cand);
            return true;
        }
    }
    return false;
}

static void *sink_reserve(void *ctx, size_t *avail)
{
    return sdw_reserve(ctx, avail);
}

static void sink_commit(void *ctx, size_t n)
{
    sdw_commit(ctx, n);
}

bool download_extract(const config_t *cfg, const char *folder, const char *zip_name,
                      zip_progress_fn cb, void *ctx, extract_result_t *res)
{
    memset(res, 0, sizeof(*res));
    uint32_t t0 = net_now_ms();

    char zip_path[CFG_PATH_MAX + 256];
    download_target(cfg, folder, zip_name, zip_path, sizeof(zip_path));
    FILE *in = fopen(zip_path, "rb");
    if (!in)
    {
        snprintf(res->error, sizeof(res->error), "can't open the archive");
        return false;
    }
    setvbuf(in, NULL, _IONBF, 0);

    zip_entry_t e;
    if (!zip_find_rom(in, rom_exts(folder), &e, res->error, sizeof(res->error)))
    {
        fclose(in);
        return false;
    }
    snprintf(res->entry, sizeof(res->entry), "%s", e.name);
    res->entries = e.entries;
    res->in_bytes = e.comp_size;
    res->out_bytes = e.size;
    if (e.size == 0 || e.size > 0xFFFFFFFFull)
    {
        fclose(in);
        snprintf(res->error, sizeof(res->error), "unsupported size");
        return false;
    }

    // Output: "<zip stem>.<ROM extension>" (save files follow the ROM name,
    // the same stem as the archive on the other clients)
    const char *dot = strrchr(e.name, '.');
    int slen = (int)(strrchr(zip_name, '.') - zip_name);
    char out_name[256];
    snprintf(out_name, sizeof(out_name), "%.*s.%s", slen, zip_name, dot + 1);
    for (char *p = strrchr(out_name, '.'); *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    download_target(cfg, folder, out_name, res->path, sizeof(res->path));
    char part[sizeof(res->path) + 8];
    snprintf(part, sizeof(part), "%s.part", res->path);

    static sdraw_extent_t ext[64];
    int n_ext = 0;
    FILE *out = open_output(cfg, part, e.size, ext, 64, &n_ext, &res->raw,
                            res->error, sizeof(res->error));
    if (!out)
    {
        fclose(in);
        return false;
    }

    sdwriter_t *w = res->raw ? sdw_open_raw(out, RING_SIZE, WRITE_CHUNK, ext, n_ext)
                             : sdw_open(out, RING_SIZE, WRITE_CHUNK);
    bool ok = false;
    if (!w)
        snprintf(res->error, sizeof(res->error), "SD writer init failed");
    else
    {
        zip_sink_t sink = { sink_reserve, sink_commit, w };
        ok = zip_extract(in, &e, &sink, cb, ctx, &res->st);
        bool wok = sdw_close(w);
        if (!ok)
        {
            snprintf(res->error, sizeof(res->error), "%s", res->st.error);
            res->cancelled = res->st.cancelled;
        }
        else if (!wok)
        {
            snprintf(res->error, sizeof(res->error), "SD write failed");
            ok = false;
        }
    }
    fclose(out);
    fclose(in);

    if (ok)
    {
        remove(res->path); // Replace an older copy
        if (rename(part, res->path) == 0)
        {
            res->ok = true;
            remove(zip_path);
        }
        else
            snprintf(res->error, sizeof(res->error), "rename failed (%d)", errno);
    }
    if (!res->ok)
        remove(part);
    // A corrupt archive is useless; keep it otherwise (retry without download)
    if (!res->ok && (strncmp(res->error, "CRC", 3) == 0 ||
                     strncmp(res->error, "inflate", 7) == 0))
    {
        remove(zip_path);
        res->zip_removed = true;
    }

    res->ms = net_now_ms() - t0;
    return res->ok;
}

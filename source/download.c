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

    FILE *f = fopen(part, "w+b");
    if (!f)
    {
        snprintf(res->error, sizeof(res->error), "can't create %.140s", part);
        return false;
    }
    setvbuf(f, NULL, _IONBF, 0); // The SD writer writes in large chunks

    static sdraw_extent_t ext[64];
    int n_ext = -1;
    res->raw = cfg->sdraw && sdraw_is_dspico();
    if (res->raw)
    {
        n_ext = sdraw_map(f, (uint32_t)size, ext, 64);
        if (n_ext <= 0)
        {
            log_printf("  raw SD writes unavailable (%d extents), using DLDI\n", n_ext);
            res->raw = false;
        }
    }
    if (!res->raw && !preallocate(f, size))
    {
        fclose(f);
        remove(part);
        snprintf(res->error, sizeof(res->error), "not enough space on the SD card");
        return false;
    }

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

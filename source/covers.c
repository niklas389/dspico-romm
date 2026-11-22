// SPDX-License-Identifier: MIT
#include "covers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"
#include "net.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

static void cover_path(const config_t *cfg, int rom_id, const char *ext, char *out, int len)
{
    char name[48];
    snprintf(name, sizeof(name), "covers/%d.%s", rom_id, ext);
    config_path(cfg, name, out, len);
}

int cover_state(const config_t *cfg, const rom_entry_t *e)
{
    char p[CFG_PATH_MAX + 48];
    struct stat st;
    cover_path(cfg, e->id, "cov", p, sizeof(p));
    if (stat(p, &st) == 0)
        return COV_CACHED;
    if (!e->cover || !e->cover[0])
        return COV_NONE;
    cover_path(cfg, e->id, "none", p, sizeof(p));
    if (stat(p, &st) == 0)
        return COV_NONE;
    return COV_MISSING;
}

static void mark_none(const config_t *cfg, int rom_id)
{
    char p[CFG_PATH_MAX + 48];
    cover_path(cfg, rom_id, "none", p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (f)
        fclose(f);
}

int cover_fetch(const config_t *cfg, const rom_entry_t *e)
{
    if (!e->cover || !e->cover[0])
        return COV_NONE;

    // Server-relative path; RomM's "?ts=2026-11-16 21:32:25" has a space
    char path[512];
    int n = 0;
    for (const char *p = e->cover; *p && n < (int)sizeof(path) - 4; p++)
    {
        if (*p == ' ')
        {
            memcpy(path + n, "%20", 3);
            n += 3;
        }
        else
            path[n++] = *p;
    }
    path[n] = 0;

    buf_t body = { 0 };
    net_result_t res;
    bool ok = net_get(path, true, &body, &res);
    if (!ok || res.status != 200 || !body.data)
    {
        log_printf("[Cover] %d: HTTP %ld %s\n", e->id, res.status, res.error);
        buf_free(&body);
        // 404: no cover; other errors: try again later
        if (res.status == 404)
        {
            mark_none(cfg, e->id);
            return COV_NONE;
        }
        return COV_MISSING;
    }

    int w, h, comp;
    uint8_t *rgb = stbi_load_from_memory((const stbi_uc *)body.data, (int)body.len, &w, &h,
                                         &comp, 3);
    size_t in_len = body.len;
    buf_free(&body);
    if (!rgb || w < 8 || h < 8)
    {
        log_printf("[Cover] %d: cannot decode %u B (not PNG/JPEG?)\n", e->id,
                   (unsigned)in_len);
        stbi_image_free(rgb);
        mark_none(cfg, e->id);
        return COV_NONE;
    }

    static uint16_t top[COVER_W * COVER_H], tile[TILE_S * TILE_S];
    cover_convert(rgb, w, h, top, tile);
    stbi_image_free(rgb);

    char dir[CFG_PATH_MAX + 16], p[CFG_PATH_MAX + 48], tmp[CFG_PATH_MAX + 56];
    config_path(cfg, "covers", dir, sizeof(dir));
    mkdir(dir, 0777);
    cover_path(cfg, e->id, "cov", p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return COV_MISSING;
    bool wr = fwrite("RCV1", 1, 4, f) == 4 && fwrite(top, sizeof(top), 1, f) == 1 &&
              fwrite(tile, sizeof(tile), 1, f) == 1;
    wr &= fclose(f) == 0;
    if (!wr || rename(tmp, p) != 0)
    {
        remove(tmp);
        return COV_MISSING;
    }
    return COV_CACHED;
}

static bool load_part(const config_t *cfg, int rom_id, long off, uint16_t *out, size_t n)
{
    char p[CFG_PATH_MAX + 48];
    cover_path(cfg, rom_id, "cov", p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f)
        return false;
    char magic[4];
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "RCV1", 4) == 0 &&
              fseek(f, off, SEEK_SET) == 0 && fread(out, 2, n, f) == n;
    fclose(f);
    return ok;
}

bool cover_load_tile(const config_t *cfg, int rom_id, uint16_t *out)
{
    return load_part(cfg, rom_id, 4 + COVER_W * COVER_H * 2, out, TILE_S * TILE_S);
}

bool cover_load_top(const config_t *cfg, int rom_id, uint16_t *out)
{
    return load_part(cfg, rom_id, 4, out, COVER_W * COVER_H);
}

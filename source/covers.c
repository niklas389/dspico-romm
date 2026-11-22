// SPDX-License-Identifier: MIT
#include "covers.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <nds.h>

#include "log.h"
#include "net.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

static const char *const ext_img[2] = { "tl", "tp" };
static const char *const ext_none[2] = { "tln", "tpn" };

static void plat_dir(const config_t *cfg, int platform_id, char *out, int len)
{
    char name[40];
    snprintf(name, sizeof(name), "covers/%d", platform_id);
    config_path(cfg, name, out, len);
}

static void file_path(const config_t *cfg, int platform_id, int rom_id, const char *ext,
                      char *out, int len)
{
    char name[64];
    snprintf(name, sizeof(name), "covers/%d/%d.%s", platform_id, rom_id, ext);
    config_path(cfg, name, out, len);
}

typedef struct
{
    int id, idx;
} id_idx_t;

static int cmp_id(const void *a, const void *b)
{
    return ((const id_idx_t *)a)->id - ((const id_idx_t *)b)->id;
}

void cover_scan(const config_t *cfg, int platform_id, const rom_entry_t *e, int n,
                uint8_t *tile, uint8_t *top)
{
    for (int i = 0; i < n; i++)
    {
        bool has = e[i].cover && e[i].cover[0];
        tile[i] = top[i] = has ? COV_MISSING : COV_NONE;
    }
    id_idx_t *map = malloc(n * sizeof(id_idx_t));
    if (!map)
        return;
    for (int i = 0; i < n; i++)
        map[i] = (id_idx_t){ e[i].id, i };
    qsort(map, n, sizeof(id_idx_t), cmp_id);

    char dir[CFG_PATH_MAX + 32];
    plat_dir(cfg, platform_id, dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (d)
    {
        struct dirent *de;
        int count = 0;
        while ((de = readdir(d)) != NULL)
        {
            if (++count % 16 == 0)
                cothread_yield(); // Keep the UI moving in big folders
            char *dot = strchr(de->d_name, '.');
            if (!dot)
                continue;
            id_idx_t key = { atoi(de->d_name), 0 };
            id_idx_t *m = bsearch(&key, map, n, sizeof(id_idx_t), cmp_id);
            if (!m)
                continue;
            const char *x = dot + 1;
            if (!strcmp(x, "tl"))
                tile[m->idx] = COV_CACHED;
            else if (!strcmp(x, "tp"))
                top[m->idx] = COV_CACHED;
            else if (!strcmp(x, "tln") && tile[m->idx] != COV_CACHED)
                tile[m->idx] = COV_NONE;
            else if (!strcmp(x, "tpn") && top[m->idx] != COV_CACHED)
                top[m->idx] = COV_NONE;
        }
        closedir(d);
    }
    free(map);
}

static void mark_none(const config_t *cfg, int platform_id, int rom_id, int kind)
{
    char p[CFG_PATH_MAX + 64];
    file_path(cfg, platform_id, rom_id, ext_none[kind], p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (f)
        fclose(f);
}

// GET into memory; true for HTTP 200 with a body. *status: HTTP status.
static bool fetch(const char *srv_path, buf_t *body, long *status)
{
    // Server-relative path; RomM's "?ts=2026-11-16 21:32:25" has a space
    char path[512];
    int n = 0;
    for (const char *p = srv_path; *p && n < (int)sizeof(path) - 4; p++)
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
    net_result_t res;
    bool ok = net_get(path, true, body, &res);
    *status = res.status;
    if (!ok || res.status != 200 || !body->data)
    {
        buf_free(body);
        return false;
    }
    return true;
}

// Decodes PNG/JPEG; NULL if not decodable or unreasonably large
static uint8_t *decode(const buf_t *b, int *w, int *h)
{
    int comp;
    if (!stbi_info_from_memory((const stbi_uc *)b->data, (int)b->len, w, h, &comp) ||
        (long)*w * *h > 1600L * 1600L || *w < 8 || *h < 8)
        return NULL;
    return stbi_load_from_memory((const stbi_uc *)b->data, (int)b->len, w, h, &comp, 3);
}

// Downloads and decodes one source; *status: HTTP status (0: not tried)
static uint8_t *get_image(const char *path, int *w, int *h, long *status)
{
    *status = 0;
    if (!path || !path[0])
        return NULL;
    buf_t body = { 0 };
    if (!fetch(path, &body, status))
        return NULL;
    uint8_t *rgb = decode(&body, w, h);
    if (!rgb)
        *status = 415; // Not decodable: treat like "no artwork"
    buf_free(&body);
    return rgb;
}

int cover_fetch(const config_t *cfg, int platform_id, const rom_entry_t *e, int kind)
{
    if (!e->cover || !e->cover[0])
        return COV_NONE;

    // 2D box art: ".../roms/<platform>/<rom>/cover/small.png?ts=..." ->
    // ".../roms/<platform>/<rom>/box2d/box2d.png"
    char box[400] = "";
    const char *c = strstr(e->cover, "/cover/");
    if (c)
        snprintf(box, sizeof(box), "%.*s/box2d/box2d.png", (int)(c - e->cover), e->cover);

    // Strip: box art first; top screen: cover first
    const char *first = kind == COV_TILE ? box : e->cover;
    const char *second = kind == COV_TILE ? e->cover : box;
    int w = 0, h = 0;
    long st1, st2 = 0;
    bool from_first = true;
    uint8_t *rgb = get_image(first, &w, &h, &st1);
    if (!rgb)
    {
        from_first = false;
        rgb = get_image(second, &w, &h, &st2);
    }
    if (!rgb)
    {
        log_printf("[Cover] %d %s: HTTP %ld / %ld\n", e->id, ext_img[kind], st1, st2);
        // Nothing usable (404, not decodable): don't ask again. Server or
        // network errors are retried in a later session.
        bool final = (st1 == 404 || st1 == 415 || st1 == 0) &&
                     (st2 == 404 || st2 == 415 || st2 == 0);
        if (final)
        {
            char dir[CFG_PATH_MAX + 32];
            config_path(cfg, "covers", dir, sizeof(dir));
            mkdir(dir, 0777);
            plat_dir(cfg, platform_id, dir, sizeof(dir));
            mkdir(dir, 0777);
            mark_none(cfg, platform_id, e->id, kind);
            return COV_NONE;
        }
        return COV_MISSING;
    }

    static uint16_t px[COVER_W * COVER_H];
    int ow, oh;
    if (kind == COV_TILE)
        cover_fit(rgb, w, h, px, TILE_S, TILE_S, &ow, &oh);
    else if (from_first)
    {
        cover_fill(rgb, w, h, px, COVER_W, COVER_H);
        ow = COVER_W;
        oh = COVER_H;
    }
    else
        cover_fit(rgb, w, h, px, COVER_W, COVER_H, &ow, &oh);
    stbi_image_free(rgb);
    log_file_only("[Cover] %d %s: %dx%d\n", e->id, ext_img[kind], w, h);

    char dir[CFG_PATH_MAX + 32], p[CFG_PATH_MAX + 64], tmp[CFG_PATH_MAX + 72];
    config_path(cfg, "covers", dir, sizeof(dir));
    mkdir(dir, 0777);
    plat_dir(cfg, platform_id, dir, sizeof(dir));
    mkdir(dir, 0777);
    file_path(cfg, platform_id, e->id, ext_img[kind], p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return COV_MISSING;
    unsigned char hdr[8] = { 'R', 'C', 'V', '3', (unsigned char)ow, (unsigned char)oh, 0, 0 };
    bool wr = fwrite(hdr, 1, 8, f) == 8 && fwrite(px, 2, ow * oh, f) == (size_t)(ow * oh);
    wr &= fclose(f) == 0;
    if (!wr || rename(tmp, p) != 0)
    {
        remove(tmp);
        return COV_MISSING;
    }
    return COV_CACHED;
}

bool cover_load(const config_t *cfg, int platform_id, int rom_id, int kind, uint16_t *out,
                int *w, int *h)
{
    char p[CFG_PATH_MAX + 64];
    file_path(cfg, platform_id, rom_id, ext_img[kind], p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f)
        return false;
    int maxw = kind == COV_TILE ? TILE_S : COVER_W, maxh = kind == COV_TILE ? TILE_S : COVER_H;
    unsigned char hdr[8];
    bool ok = fread(hdr, 1, 8, f) == 8 && memcmp(hdr, "RCV3", 4) == 0 && hdr[4] && hdr[5] &&
              hdr[4] <= maxw && hdr[5] <= maxh;
    if (ok)
    {
        *w = hdr[4];
        *h = hdr[5];
        size_t n = (size_t)(*w * *h);
        ok = fread(out, 2, n, f) == n;
    }
    fclose(f);
    return ok;
}

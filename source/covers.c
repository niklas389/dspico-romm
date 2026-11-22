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

#include "imgdec.h"

bool cover_tile_box2d = true;

// Strip images from box art (.tl) and from the cover (.tc) are kept apart, so
// changing art= in config.ini takes effect without deleting the cache
static const char *ext_img_k(int kind)
{
    return kind == COV_TOP ? "tp" : cover_tile_box2d ? "tl2" : "tc2";
}

static const char *ext_none_k(int kind)
{
    return kind == COV_TOP ? "tpn" : cover_tile_box2d ? "tl2n" : "tc2n";
}

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
            if (!strcmp(x, ext_img_k(COV_TILE)))
                tile[m->idx] = COV_CACHED;
            else if (!strcmp(x, "tp"))
                top[m->idx] = COV_CACHED;
            else if (!strcmp(x, ext_none_k(COV_TILE)) && tile[m->idx] != COV_CACHED)
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
    file_path(cfg, platform_id, rom_id, ext_none_k(kind), p, sizeof(p));
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

    // Strip: box art first (art=box2d) or the cover; top screen: cover first
    bool box_first = kind == COV_TILE && cover_tile_box2d;
    const char *src[2] = { box_first ? box : e->cover, box_first ? e->cover : box };
    static uint16_t px[COVER_W * COVER_H];
    int ow = 0, oh = 0;
    long st[2] = { 0, 0 };
    bool ok = false;
    int used = -1;
    for (int k = 0; k < 2 && !ok; k++)
    {
        if (!src[k][0])
            continue;
        uint32_t t0 = net_now_ms();
        buf_t body = { 0 };
        if (!fetch(src[k], &body, &st[k]))
            continue;
        uint32_t t1 = net_now_ms();
        // Top screen: the cover fills 90x120; box art and strip images are
        // shown whole
        bool fill = kind == COV_TOP && src[k] == e->cover;
        // Strip: nearly square art (DS boxes are ~1.12:1) fills the whole
        // tile, cropping a little at the sides; clearly portrait or landscape
        // art is shown whole
        int iw, ih;
        if (kind == COV_TILE &&
            imgdec_size((const uint8_t *)body.data, body.len, &iw, &ih) &&
            iw * 4 <= ih * 5 && ih * 4 <= iw * 5)
            fill = true;
        imgdec_info_t inf;
        ok = imgdec_scaled((const uint8_t *)body.data, body.len, fill,
                           kind == COV_TILE ? TILE_S : COVER_W,
                           kind == COV_TILE ? TILE_S : COVER_H, px, &ow, &oh, &inf);
        uint32_t t2 = net_now_ms();
        log_printf("[Cover] %d %s %s: %u B, %dx%d %s, fetch %lu ms, decode %lu ms "
                   "(inflate %lu, rows %lu)%s\n", e->id,
                   ext_img_k(kind), src[k] == box ? "box2d" : "cover", (unsigned)body.len,
                   inf.w, inf.h, inf.how ? inf.how : "-", (unsigned long)(t1 - t0),
                   (unsigned long)(t2 - t1), (unsigned long)inf.inflate_ms,
                   (unsigned long)inf.rows_ms, ok ? "" : " FAILED");
        buf_free(&body);
        if (!ok)
            st[k] = 415; // Not decodable: like "no artwork"
        else
            used = k;
    }
    (void)used;
    if (!ok)
    {
        log_printf("[Cover] %d %s: HTTP %ld / %ld\n", e->id, ext_img_k(kind), st[0], st[1]);
        // Nothing usable (404, not decodable): don't ask again. Server or
        // network errors are retried in a later session.
        // Status 0 means "not tried" only for an empty source path; a
        // network error (no HTTP status) is never final
        bool final = true;
        for (int k = 0; k < 2; k++)
            if (src[k][0] && st[k] != 404 && st[k] != 415)
                final = false;
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

    char dir[CFG_PATH_MAX + 32], p[CFG_PATH_MAX + 64], tmp[CFG_PATH_MAX + 72];
    config_path(cfg, "covers", dir, sizeof(dir));
    mkdir(dir, 0777);
    plat_dir(cfg, platform_id, dir, sizeof(dir));
    mkdir(dir, 0777);
    file_path(cfg, platform_id, e->id, ext_img_k(kind), p, sizeof(p));
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
    file_path(cfg, platform_id, rom_id, ext_img_k(kind), p, sizeof(p));
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

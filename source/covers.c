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
    cover_path(cfg, e->id, "cv2", p, sizeof(p));
    if (stat(p, &st) == 0)
        return COV_CACHED;
    if (!e->cover || !e->cover[0])
        return COV_NONE;
    cover_path(cfg, e->id, "nn2", p, sizeof(p));
    if (stat(p, &st) == 0)
        return COV_NONE;
    return COV_MISSING;
}

static void mark_none(const config_t *cfg, int rom_id)
{
    char p[CFG_PATH_MAX + 48];
    cover_path(cfg, rom_id, "nn2", p, sizeof(p));
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

int cover_fetch(const config_t *cfg, const rom_entry_t *e)
{
    if (!e->cover || !e->cover[0])
        return COV_NONE;

    // 2D box art: ".../roms/<platform>/<rom>/cover/small.png?ts=..." ->
    // ".../roms/<platform>/<rom>/box2d/box2d.png"
    char box[400] = "";
    const char *c = strstr(e->cover, "/cover/");
    if (c)
        snprintf(box, sizeof(box), "%.*s/box2d/box2d.png", (int)(c - e->cover), e->cover);

    buf_t body = { 0 };
    long st1 = 0, st2 = 0;
    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    const char *src = "box2d";
    if (box[0] && fetch(box, &body, &st1))
    {
        rgb = decode(&body, &w, &h);
        buf_free(&body);
    }
    if (!rgb)
    {
        src = "cover";
        if (fetch(e->cover, &body, &st2))
        {
            rgb = decode(&body, &w, &h);
            if (!rgb)
                log_printf("[Cover] %d: cannot decode %u B (not PNG/JPEG?)\n", e->id,
                           (unsigned)body.len);
            buf_free(&body);
        }
        else
            log_printf("[Cover] %d: box2d HTTP %ld, cover HTTP %ld\n", e->id, st1, st2);
    }
    if (!rgb)
    {
        // No artwork (404), or not decodable: don't ask again. Other HTTP
        // errors (server, network) are retried in a later session.
        if (st2 == 404 || st2 == 200)
        {
            mark_none(cfg, e->id);
            return COV_NONE;
        }
        return COV_MISSING;
    }

    static uint16_t top[COVER_W * COVER_H], tile[TILE_S * TILE_S];
    int tw, th, sw, sh;
    cover_fit(rgb, w, h, top, COVER_W, COVER_H, &tw, &th);
    cover_fit(rgb, w, h, tile, TILE_S, TILE_S, &sw, &sh);
    stbi_image_free(rgb);
    log_file_only("[Cover] %d: %s %dx%d\n", e->id, src, w, h);

    char dir[CFG_PATH_MAX + 16], p[CFG_PATH_MAX + 48], tmp[CFG_PATH_MAX + 56];
    config_path(cfg, "covers", dir, sizeof(dir));
    mkdir(dir, 0777);
    cover_path(cfg, e->id, "cv2", p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return COV_MISSING;
    unsigned char hdr[8] = { 'R', 'C', 'V', '2', (unsigned char)tw, (unsigned char)th,
                             (unsigned char)sw, (unsigned char)sh };
    bool wr = fwrite(hdr, 1, 8, f) == 8 && fwrite(top, 2, tw * th, f) == (size_t)(tw * th) &&
              fwrite(tile, 2, sw * sh, f) == (size_t)(sw * sh);
    wr &= fclose(f) == 0;
    if (!wr || rename(tmp, p) != 0)
    {
        remove(tmp);
        return COV_MISSING;
    }
    return COV_CACHED;
}

static bool load_part(const config_t *cfg, int rom_id, bool want_tile, uint16_t *out, int *w,
                      int *h)
{
    char p[CFG_PATH_MAX + 48];
    cover_path(cfg, rom_id, "cv2", p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f)
        return false;
    unsigned char hdr[8];
    bool ok = fread(hdr, 1, 8, f) == 8 && memcmp(hdr, "RCV2", 4) == 0 && hdr[4] &&
              hdr[5] && hdr[6] && hdr[7] && hdr[4] <= COVER_W && hdr[5] <= COVER_H &&
              hdr[6] <= TILE_S && hdr[7] <= TILE_S;
    if (ok)
    {
        long off = 8 + (want_tile ? hdr[4] * hdr[5] * 2 : 0);
        *w = want_tile ? hdr[6] : hdr[4];
        *h = want_tile ? hdr[7] : hdr[5];
        size_t n = (size_t)(*w * *h);
        ok = fseek(f, off, SEEK_SET) == 0 && fread(out, 2, n, f) == n;
    }
    fclose(f);
    return ok;
}

bool cover_load_tile(const config_t *cfg, int rom_id, uint16_t *out, int *w, int *h)
{
    return load_part(cfg, rom_id, true, out, w, h);
}

bool cover_load_top(const config_t *cfg, int rom_id, uint16_t *out, int *w, int *h)
{
    return load_part(cfg, rom_id, false, out, w, h);
}

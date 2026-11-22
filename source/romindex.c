// SPDX-License-Identifier: MIT
//
// Local ROM index per platform (see romindex.h).

#include "romindex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "log.h"

#define PAGE 50
#define IDX_VERSION "#romm-index v1"

void idx_init(rom_index_t *idx, int platform_id)
{
    memset(idx, 0, sizeof(*idx));
    idx->platform_id = platform_id;
}

static void entry_free(rom_entry_t *e)
{
    free(e->name);
    free(e->fs_name);
    free(e->ext);
    free(e->updated);
    free(e->regions);
    free(e->genres);
    free(e->players);
    free(e->cover);
    memset(e, 0, sizeof(*e));
}

void idx_free(rom_index_t *idx)
{
    for (int i = 0; i < idx->count; i++)
        entry_free(&idx->e[i]);
    free(idx->e);
    idx->e = NULL;
    idx->count = idx->cap = 0;
}

// Copy without tabs and line breaks (the index is tab-separated).
static char *clean_dup(const char *s)
{
    if (!s)
        s = "";
    char *d = strdup(s);
    if (!d)
        return NULL;
    for (char *p = d; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r')
            *p = ' ';
    return d;
}

static rom_entry_t *idx_find(rom_index_t *idx, int id)
{
    for (int i = 0; i < idx->count; i++)
        if (idx->e[i].id == id)
            return &idx->e[i];
    return NULL;
}

static rom_entry_t *idx_add(rom_index_t *idx)
{
    if (idx->count == idx->cap)
    {
        int cap = idx->cap ? idx->cap * 2 : 64;
        rom_entry_t *e = realloc(idx->e, cap * sizeof(*e));
        if (!e)
            return NULL;
        idx->e = e;
        idx->cap = cap;
    }
    rom_entry_t *e = &idx->e[idx->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void path_index(const config_t *cfg, int platform_id, char *out, int len)
{
    char dir[CFG_PATH_MAX + 16];
    config_path(cfg, "index", dir, sizeof(dir));
    mkdir(dir, 0777);
    snprintf(out, len, "%s/%d.tsv", dir, platform_id);
}

static void path_meta(const config_t *cfg, int rom_id, char *out, int len)
{
    char dir[CFG_PATH_MAX + 16];
    config_path(cfg, "meta", dir, sizeof(dir));
    mkdir(dir, 0777);
    snprintf(out, len, "%s/%d.txt", dir, rom_id);
}

static int name_cmp(const void *a, const void *b)
{
    const rom_entry_t *x = a, *y = b;
    return strcasecmp(x->name ? x->name : "", y->name ? y->name : "");
}

// --- File format -------------------------------------------------------------
// Line 1: "#romm-index v1\t<watermark>"
// Then:   id size year name fs_name ext updated regions genres players cover

bool idx_save(const config_t *cfg, const rom_index_t *idx)
{
    char path[CFG_PATH_MAX + 32], tmp[CFG_PATH_MAX + 40];
    path_index(cfg, idx->platform_id, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.new", path);

    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    setvbuf(f, NULL, _IOFBF, 32 * 1024);
    fprintf(f, "%s\t%s\n", IDX_VERSION, idx->watermark);
    for (int i = 0; i < idx->count; i++)
    {
        const rom_entry_t *e = &idx->e[i];
        fprintf(f, "%d\t%lu\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", e->id,
                (unsigned long)e->size, e->year, e->name, e->fs_name, e->ext,
                e->updated, e->regions, e->genres, e->players, e->cover);
    }
    bool ok = fclose(f) == 0;
    if (ok)
    {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    return ok;
}

bool idx_load(const config_t *cfg, rom_index_t *idx)
{
    char path[CFG_PATH_MAX + 32];
    path_index(cfg, idx->platform_id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    setvbuf(f, NULL, _IOFBF, 32 * 1024);

    static char line[4096];
    bool ok = false;
    if (fgets(line, sizeof(line), f) && strncmp(line, IDX_VERSION, strlen(IDX_VERSION)) == 0)
    {
        char *wm = strchr(line, '\t');
        if (wm)
        {
            wm++;
            wm[strcspn(wm, "\r\n")] = '\0';
            snprintf(idx->watermark, sizeof(idx->watermark), "%s", wm);
        }
        ok = true;
        while (fgets(line, sizeof(line), f))
        {
            line[strcspn(line, "\r\n")] = '\0';
            char *fld[11];
            int n = 0;
            char *p = line;
            while (n < 11)
            {
                fld[n++] = p;
                char *t = strchr(p, '\t');
                if (!t)
                    break;
                *t = '\0';
                p = t + 1;
            }
            if (n < 11)
                continue;
            rom_entry_t *e = idx_add(idx);
            if (!e)
            {
                ok = false;
                break;
            }
            e->id = atoi(fld[0]);
            e->size = strtoul(fld[1], NULL, 10);
            e->year = atoi(fld[2]);
            e->name = strdup(fld[3]);
            e->fs_name = strdup(fld[4]);
            e->ext = strdup(fld[5]);
            e->updated = strdup(fld[6]);
            e->regions = strdup(fld[7]);
            e->genres = strdup(fld[8]);
            e->players = strdup(fld[9]);
            e->cover = strdup(fld[10]);
        }
    }
    fclose(f);
    return ok;
}

// --- Sync --------------------------------------------------------------------

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

// "a,b,c" from a JSON string array (at most len bytes)
static void join_list(const cJSON *arr, char *out, int len)
{
    out[0] = '\0';
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        if (!cJSON_IsString(it))
            continue;
        int w = snprintf(out + n, len - n, "%s%s", n ? "," : "", it->valuestring);
        if (w < 0 || w >= len - n)
            break;
        n += w;
    }
}

static void write_meta(const config_t *cfg, const cJSON *it, int id)
{
    const char *summary = jstr(it, "summary");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "metadatum");
    char pub[128] = "", dev[128] = "", langs[128] = "", modes[96] = "";
    if (m)
    {
        join_list(cJSON_GetObjectItemCaseSensitive(m, "publishers"), pub, sizeof(pub));
        join_list(cJSON_GetObjectItemCaseSensitive(m, "developers"), dev, sizeof(dev));
        join_list(cJSON_GetObjectItemCaseSensitive(m, "game_modes"), modes, sizeof(modes));
    }
    join_list(cJSON_GetObjectItemCaseSensitive(it, "languages"), langs, sizeof(langs));

    char path[CFG_PATH_MAX + 32];
    path_meta(cfg, id, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "publisher=%s\ndeveloper=%s\nlanguages=%s\nmodes=%s\nmd5=%s\n\n%s\n", pub, dev,
            langs, modes, jstr(it, "md5_hash") ? jstr(it, "md5_hash") : "",
            summary ? summary : "");
    fclose(f);
}

static bool apply_item(const config_t *cfg, rom_index_t *idx, const cJSON *it)
{
    int id = (int)jnum(it, "id", 0);
    if (id <= 0)
        return true;
    rom_entry_t *e = idx_find(idx, id);
    if (e)
        entry_free(e);
    else if (!(e = idx_add(idx)))
        return false;

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "metadatum");
    char regions[96], genres[96];
    join_list(cJSON_GetObjectItemCaseSensitive(it, "regions"), regions, sizeof(regions));
    join_list(m ? cJSON_GetObjectItemCaseSensitive(m, "genres") : NULL, genres, sizeof(genres));

    int year = 0;
    double rel = m ? jnum(m, "first_release_date", 0) : 0; // epoch ms
    if (rel > 0)
    {
        time_t t = (time_t)(rel / 1000);
        struct tm *tm = gmtime(&t);
        if (tm)
            year = tm->tm_year + 1900;
    }

    e->id = id;
    e->size = (uint32_t)jnum(it, "fs_size_bytes", 0);
    e->year = year;
    const char *name = jstr(it, "name");
    e->name = clean_dup(name && name[0] ? name : jstr(it, "fs_name_no_ext"));
    e->fs_name = clean_dup(jstr(it, "fs_name"));
    e->ext = clean_dup(jstr(it, "fs_extension"));
    e->updated = clean_dup(jstr(it, "updated_at"));
    e->regions = clean_dup(regions);
    e->genres = clean_dup(genres);
    e->players = clean_dup(m ? jstr(m, "player_count") : NULL);
    e->cover = clean_dup(jstr(it, "path_cover_small"));
    if (!e->name || !e->fs_name || !e->ext || !e->updated || !e->regions || !e->genres ||
        !e->players || !e->cover)
        return false;

    if (strcmp(e->updated, idx->watermark) > 0)
        snprintf(idx->watermark, sizeof(idx->watermark), "%s", e->updated);
    write_meta(cfg, it, id);
    return true;
}

// '+' in the timestamp must not turn into a space in the query string.
static void url_time(const char *in, char *out, int len)
{
    int n = 0;
    for (const char *p = in; *p && n < len - 4; p++)
    {
        if (*p == '+')
            n += snprintf(out + n, len - n, "%%2B");
        else if (*p == ':')
            n += snprintf(out + n, len - n, "%%3A");
        else
            out[n++] = *p;
    }
    out[n] = '\0';
}

bool idx_sync(const config_t *cfg, rom_index_t *idx, idx_progress_fn cb,
              idx_sync_result_t *res)
{
    (void)cfg;
    memset(res, 0, sizeof(*res));
    uint32_t t0 = net_now_ms();
    res->full = idx->count == 0 || !idx->watermark[0];
    if (res->full)
        idx->watermark[0] = '\0';

    char since[64] = "";
    if (!res->full)
        url_time(idx->watermark, since, sizeof(since));

    int offset = 0, total = 0;
    while (1)
    {
        char path[512];
        snprintf(path, sizeof(path),
                 "/api/roms?platform_ids=%d&limit=%d&offset=%d&order_by=name&order_dir=asc"
                 "&with_provider_metadata=false&with_char_index=false"
                 "&with_filter_values=false&with_rom_id_index=false%s%s",
                 idx->platform_id, PAGE, offset, since[0] ? "&updated_after=" : "", since);
        buf_t body = { 0 };
        net_result_t r;
        res->requests++;
        bool ok = net_get(path, true, &body, &r);
        if (!ok || r.status != 200)
        {
            snprintf(res->error, sizeof(res->error), "ROM list: HTTP %ld %.80s", r.status,
                     r.error);
            buf_free(&body);
            return false;
        }
        cJSON *root = cJSON_Parse(body.data);
        buf_free(&body);
        if (!root)
        {
            snprintf(res->error, sizeof(res->error), "ROM list: bad JSON");
            return false;
        }
        total = (int)jnum(root, "total", total);
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
        int n = cJSON_GetArraySize(items);
        const cJSON *it;
        bool mem_ok = true;
        cJSON_ArrayForEach(it, items)
            if (!(mem_ok = apply_item(cfg, idx, it)))
                break;
        cJSON_Delete(root);
        if (!mem_ok)
        {
            snprintf(res->error, sizeof(res->error), "out of memory");
            return false;
        }
        res->fetched += n;
        offset += n;
        if (cb)
            cb(res->fetched, total);
        if (n < PAGE || offset >= total)
            break;
    }

    // Drop ROMs that no longer exist (incremental syncs only see changes).
    if (!res->full)
    {
        buf_t body = { 0 };
        net_result_t r;
        res->requests++;
        if (net_get("/api/roms/identifiers", true, &body, &r) && r.status == 200)
        {
            cJSON *ids = cJSON_Parse(body.data);
            int n = cJSON_GetArraySize(ids);
            if (ids && n > 0)
            {
                int *set = malloc(n * sizeof(int));
                if (set)
                {
                    int k = 0;
                    const cJSON *v;
                    cJSON_ArrayForEach(v, ids)
                        if (cJSON_IsNumber(v))
                            set[k++] = v->valueint;
                    for (int i = 0; i < idx->count;)
                    {
                        bool found = false;
                        for (int j = 0; j < k && !found; j++)
                            found = set[j] == idx->e[i].id;
                        if (found)
                        {
                            i++;
                            continue;
                        }
                        entry_free(&idx->e[i]);
                        idx->e[i] = idx->e[--idx->count];
                        res->removed++;
                    }
                    free(set);
                }
            }
            cJSON_Delete(ids);
        }
        buf_free(&body);
    }

    qsort(idx->e, idx->count, sizeof(rom_entry_t), name_cmp);
    res->ms = net_now_ms() - t0;
    if (!idx_save(cfg, idx))
    {
        snprintf(res->error, sizeof(res->error), "can't save the index");
        return false;
    }
    return true;
}

bool idx_meta(const config_t *cfg, int rom_id, char *buf, int len)
{
    char path[CFG_PATH_MAX + 32];
    path_meta(cfg, rom_id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, len - 1, f);
    buf[n] = '\0';
    fclose(f);
    return true;
}

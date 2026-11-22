// SPDX-License-Identifier: MIT
//
// Save sync with RomM (see savesync.h).

#include "savesync.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <cJSON.h>
#include <mbedtls/md5.h>
#include <nds.h>
#include <dswifi9.h>
#include <nds.h>

#include "download.h"
#include "log.h"
#include "net.h"

#define SAVE_MAX (4 * 1024 * 1024)

static char g_device_id[64];

const char *ss_device_id(void)
{
    return g_device_id;
}

const char *ss_action_name(int action)
{
    switch (action)
    {
    case SS_NONE: return "no save";
    case SS_IN_SYNC: return "in sync";
    case SS_UPLOAD: return "DSi newer";
    case SS_DOWNLOAD: return "RomM newer";
    case SS_CONFLICT: return "CONFLICT";
    default: return "error";
    }
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void lower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

static void md5_hex(const void *data, size_t len, char out[33])
{
    unsigned char d[16];
    mbedtls_md5(data, len, d);
    for (int i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

// MD5 of a file read in 16 KB pieces, yielding to the other threads after
// each (the save status is computed by a background thread while the UI runs)
static bool file_md5(const char *path, char out[33], size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    static uint8_t buf[16 * 1024];
    mbedtls_md5_context c;
    mbedtls_md5_init(&c);
    mbedtls_md5_starts(&c);
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        mbedtls_md5_update(&c, buf, n);
        total += n;
        cothread_yield();
        if (total > SAVE_MAX)
            break;
    }
    bool ok = !ferror(f) && total <= SAVE_MAX;
    fclose(f);
    unsigned char d[16];
    mbedtls_md5_finish(&c, d);
    mbedtls_md5_free(&c);
    if (!ok)
        return false;
    for (int i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
    *len = total;
    return true;
}

static bool read_file(const char *path, uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > SAVE_MAX)
    {
        fclose(f);
        return false;
    }
    *data = malloc(n ? (size_t)n : 1);
    bool ok = *data && fread(*data, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok)
    {
        free(*data);
        *data = NULL;
        return false;
    }
    *len = (size_t)n;
    return true;
}

static bool write_file(const char *path, const void *data, size_t len)
{
    char tmp[CFG_PATH_MAX + 300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
    {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

// --- Device id ----------------------------------------------------------------

static void device_ident(char *out, int len)
{
    unsigned char mac[6] = { 0 };
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);
    snprintf(out, len, "dsi-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
}

bool ss_init(const config_t *cfg)
{
    if (g_device_id[0])
        return true;
    char path[CFG_PATH_MAX + 32];
    config_path(cfg, "device.txt", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fgets(g_device_id, sizeof(g_device_id), f))
            g_device_id[strcspn(g_device_id, "\r\n")] = '\0';
        fclose(f);
        if (g_device_id[0])
            return true;
    }

    // The pairing created a device with our identifier (dsi-<MAC>)
    char ident[32];
    device_ident(ident, sizeof(ident));
    buf_t body = { 0 };
    net_result_t res;
    bool ok = net_get("/api/devices", true, &body, &res);
    if (ok && res.status == 200)
    {
        cJSON *arr = cJSON_Parse(body.data);
        const cJSON *it;
        cJSON_ArrayForEach(it, arr)
        {
            const char *ci = jstr(it, "client_device_identifier");
            const char *id = jstr(it, "id");
            if (ci && id && strcmp(ci, ident) == 0)
            {
                snprintf(g_device_id, sizeof(g_device_id), "%s", id);
                break;
            }
        }
        cJSON_Delete(arr);
    }
    buf_free(&body);

    if (!g_device_id[0])
    {
        // Not found: register one
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "name", "DSi (DSPico)");
        cJSON_AddStringToObject(req, "platform", "nds");
        cJSON_AddStringToObject(req, "client", "dspico-romm");
        cJSON_AddStringToObject(req, "client_version", POC_VERSION);
        cJSON_AddBoolToObject(req, "allow_existing", true);
        char *json = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        ok = net_post_json("/api/devices", json, true, &body, &res);
        free(json);
        if (ok && (res.status == 200 || res.status == 201))
        {
            cJSON *o = cJSON_Parse(body.data);
            const char *id = jstr(o, "device_id");
            if (id)
                snprintf(g_device_id, sizeof(g_device_id), "%s", id);
            cJSON_Delete(o);
        }
        buf_free(&body);
    }

    if (!g_device_id[0])
    {
        log_printf("Save sync: no RomM device id (HTTP %ld)\n", res.status);
        return false;
    }
    f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%s\n", g_device_id);
        fclose(f);
    }
    log_printf("Save sync: device %s\n", g_device_id);
    return true;
}

// --- Server saves -------------------------------------------------------------

void ss_list_free(ss_list_t *l)
{
    free(l->v);
    l->v = NULL;
    l->count = 0;
}

static void parse_save(const cJSON *it, ss_server_save_t *s)
{
    memset(s, 0, sizeof(*s));
    s->id = (int)jnum(it, "id");
    s->rom_id = (int)jnum(it, "rom_id");
    s->size = (uint32_t)jnum(it, "file_size_bytes");
    const char *v;
    if ((v = jstr(it, "updated_at")))
        snprintf(s->updated_at, sizeof(s->updated_at), "%s", v);
    if ((v = jstr(it, "content_hash")))
    {
        snprintf(s->hash, sizeof(s->hash), "%s", v);
        lower(s->hash);
    }
    if ((v = jstr(it, "file_name")))
        snprintf(s->file_name, sizeof(s->file_name), "%s", v);
    const cJSON *ds = cJSON_GetObjectItemCaseSensitive(it, "device_syncs");
    const cJSON *d;
    cJSON_ArrayForEach(d, ds)
    {
        const char *id = jstr(d, "device_id");
        if (id && strcmp(id, g_device_id) == 0)
            s->current = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "is_current"));
    }
}

bool ss_fetch(const config_t *cfg, int platform_id, int rom_id, ss_list_t *out)
{
    memset(out, 0, sizeof(*out));
    char path[256];
    char *slot = net_escape(cfg->save_slot);
    if (rom_id)
        snprintf(path, sizeof(path), "/api/saves?rom_id=%d&device_id=%s&slot=%s", rom_id,
                 g_device_id, slot ? slot : "");
    else
        snprintf(path, sizeof(path), "/api/saves?platform_id=%d&device_id=%s&slot=%s",
                 platform_id, g_device_id, slot ? slot : "");
    free(slot);

    buf_t body = { 0 };
    net_result_t res;
    bool ok = net_get(path, true, &body, &res) && res.status == 200;
    if (!ok)
    {
        log_printf("Save sync: GET saves failed (HTTP %ld) %s\n", res.status, res.error);
        buf_free(&body);
        return false;
    }
    cJSON *arr = cJSON_Parse(body.data);
    buf_free(&body);
    int n = cJSON_GetArraySize(arr);
    out->v = calloc(n ? n : 1, sizeof(ss_server_save_t));
    if (!out->v)
    {
        cJSON_Delete(arr);
        return false;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        // Slot filter on our side too (null-slot saves are manual uploads)
        const char *sl = jstr(it, "slot");
        if (!sl || strcasecmp(sl, cfg->save_slot) != 0)
            continue;
        parse_save(it, &out->v[out->count++]);
    }
    cJSON_Delete(arr);
    return true;
}

// --- Sync state per ROM ---------------------------------------------------------

static void base_path(const config_t *cfg, int rom_id, char *out, int len)
{
    char name[48];
    snprintf(name, sizeof(name), "saves/%d.txt", rom_id);
    config_path(cfg, name, out, len);
}

static void base_load(const config_t *cfg, ss_status_t *st)
{
    char path[CFG_PATH_MAX + 48], line[96];
    base_path(cfg, st->rom_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "local=", 6) == 0)
            snprintf(st->base_local, sizeof(st->base_local), "%.32s", line + 6);
        else if (strncmp(line, "server=", 7) == 0)
            snprintf(st->base_server, sizeof(st->base_server), "%.47s", line + 7);
    }
    fclose(f);
}

static void base_save(const config_t *cfg, int rom_id, const char *local,
                      const char *server, int save_id)
{
    char dir[CFG_PATH_MAX + 16], path[CFG_PATH_MAX + 48];
    config_path(cfg, "saves", dir, sizeof(dir));
    mkdir(dir, 0777);
    base_path(cfg, rom_id, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "local=%s\nserver=%s\nsave_id=%d\n", local, server, save_id);
    fclose(f);
}

// --- Status -----------------------------------------------------------------------

void ss_status(const config_t *cfg, const rom_entry_t *e, const char *folder,
               const ss_list_t *server, ss_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->rom_id = e->id;

    // <rom stem>.sav next to the ROM (Pico Launcher)
    char rom_name[256];
    if (!download_local(cfg, folder, e->fs_name, rom_name, sizeof(rom_name), NULL))
    {
        st->action = SS_NONE; // ROM not on the SD card: nothing to sync
        return;
    }
    char *dot = strrchr(rom_name, '.');
    if (dot)
        *dot = '\0';
    char sav[300];
    snprintf(sav, sizeof(sav), "%s.sav", rom_name);
    download_target(cfg, folder, sav, st->local_path, sizeof(st->local_path));

    struct stat sb;
    if (stat(st->local_path, &sb) == 0)
    {
        size_t len;
        if (file_md5(st->local_path, st->local_hash, &len))
        {
            st->have_local = true;
            st->local_size = (uint32_t)len;
            st->local_mtime = sb.st_mtime;
        }
    }

    // Newest server save of this ROM in the slot
    for (int i = 0; server && i < server->count; i++)
    {
        const ss_server_save_t *s = &server->v[i];
        if (s->rom_id != e->id)
            continue;
        if (!st->have_server || strcmp(s->updated_at, st->srv.updated_at) > 0)
        {
            st->srv = *s;
            st->have_server = true;
        }
    }

    base_load(cfg, st);

    st->action = ss_decide(st->have_local, st->have_server, st->local_hash, st->srv.hash,
                           st->srv.current, st->base_local, st->base_server);
}

// --- Transfers ------------------------------------------------------------------

// DeSmuME appends a 122-byte footer ("|<--Snip above here ... |-DESMUME SAVE-|")
static size_t strip_desmume_footer(const uint8_t *d, size_t len)
{
    static const char end[] = "|-DESMUME SAVE-|";
    static const char snip[] = "|<--Snip above here";
    if (len < 16 || memcmp(d + len - 16, end, 16) != 0)
        return len;
    size_t from = len > 256 ? len - 256 : 0;
    for (size_t i = len - 16; i-- > from;)
        if (memcmp(d + i, snip, sizeof(snip) - 1) == 0)
            return i;
    return len;
}

static bool backup_local(const config_t *cfg, const ss_status_t *st)
{
    if (!st->have_local)
        return true;
    char dir[CFG_PATH_MAX + 16];
    config_path(cfg, "save-backup", dir, sizeof(dir));
    mkdir(dir, 0777);
    const char *base = strrchr(st->local_path, '/');
    base = base ? base + 1 : st->local_path;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char name[300], path[CFG_PATH_MAX + 340];
    snprintf(name, sizeof(name), "save-backup/%04d%02d%02d-%02d%02d%02d %.240s",
             (tm->tm_year + 1900) % 10000, (tm->tm_mon + 1) % 100, tm->tm_mday % 100,
             tm->tm_hour % 100, tm->tm_min % 100, tm->tm_sec % 100, base);
    config_path(cfg, name, path, sizeof(path));
    uint8_t *data;
    size_t len;
    if (!read_file(st->local_path, &data, &len))
        return false;
    bool ok = write_file(path, data, len);
    free(data);
    return ok;
}

static bool do_download(const config_t *cfg, const ss_status_t *st, char *err, int err_len)
{
    char path[160];
    snprintf(path, sizeof(path), "/api/saves/%d/content?device_id=%s", st->srv.id,
             g_device_id);
    buf_t body = { 0 };
    net_result_t res;
    bool ok = net_get(path, true, &body, &res) && res.status == 200;
    if (!ok)
    {
        snprintf(err, err_len, res.status == 409 ? "RomM: conflict (HTTP 409)"
                                                 : "download failed (HTTP %ld)",
                 res.status);
        buf_free(&body);
        return false;
    }
    size_t len = strip_desmume_footer((const uint8_t *)body.data, body.len);
    if (len != body.len)
        log_printf("  removed DeSmuME footer (%u -> %u B)\n", (unsigned)body.len,
                   (unsigned)len);

    char md5[33];
    md5_hex(body.data, body.len, md5);
    log_printf("  server hash %s, MD5 of the download %s (%s)\n",
               st->srv.hash[0] ? st->srv.hash : "-", md5,
               strcmp(md5, st->srv.hash) == 0 ? "MD5" : "different scheme");
    if (st->have_local && st->local_size != len)
        log_printf("  size differs: local %lu B, server %u B\n",
                   (unsigned long)st->local_size, (unsigned)len);

    if (!backup_local(cfg, st))
    {
        snprintf(err, err_len, "backup of the local save failed");
        buf_free(&body);
        return false;
    }
    if (!write_file(st->local_path, body.data, len))
    {
        snprintf(err, err_len, "writing the save failed");
        buf_free(&body);
        return false;
    }
    char local[33];
    md5_hex(body.data, len, local);
    buf_free(&body);

    // Tell RomM this device has the version now
    char json[160];
    snprintf(json, sizeof(json), "{\"device_id\":\"%s\",\"content_hash\":\"%s\"}",
             g_device_id, st->srv.hash[0] ? st->srv.hash : md5);
    snprintf(path, sizeof(path), "/api/saves/%d/downloaded", st->srv.id);
    if (!net_post_json(path, json, true, &body, &res) || res.status != 200)
        log_printf("  confirm download: HTTP %ld\n", res.status);
    buf_free(&body);

    base_save(cfg, st->rom_id, local, st->srv.hash[0] ? st->srv.hash : md5, st->srv.id);
    return true;
}

static bool do_upload(const config_t *cfg, const ss_status_t *st, char *err, int err_len)
{
    uint8_t *data;
    size_t len;
    if (!read_file(st->local_path, &data, &len))
    {
        snprintf(err, err_len, "can't read the local save");
        return false;
    }
    const char *fname = strrchr(st->local_path, '/');
    fname = fname ? fname + 1 : st->local_path;

    char path[320];
    char *slot = net_escape(cfg->save_slot);
    if (st->have_server)
        snprintf(path, sizeof(path), "/api/saves/%d?device_id=%s&content_hash=%s",
                 st->srv.id, g_device_id, st->local_hash);
    else
        snprintf(path, sizeof(path),
                 "/api/saves?rom_id=%d&slot=%s&device_id=%s&content_hash=%s",
                 st->rom_id, slot ? slot : "", g_device_id, st->local_hash);
    free(slot);

    buf_t body = { 0 };
    net_result_t res;
    bool ok = net_send_file(st->have_server ? "PUT" : "POST", path, "saveFile", fname,
                            data, len, &body, &res) &&
              res.status == 200;
    free(data);
    if (!ok)
    {
        snprintf(err, err_len, "upload failed (HTTP %ld) %.80s", res.status, res.error);
        buf_free(&body);
        return false;
    }
    cJSON *o = cJSON_Parse(body.data);
    ss_server_save_t s;
    parse_save(o, &s);
    cJSON_Delete(o);
    buf_free(&body);
    log_printf("  uploaded as save %d, server hash %s\n", s.id, s.hash[0] ? s.hash : "-");
    base_save(cfg, st->rom_id, st->local_hash, s.hash[0] ? s.hash : st->local_hash,
              s.id ? s.id : st->srv.id);
    return true;
}

bool ss_apply(const config_t *cfg, const ss_status_t *st, int action, char *err,
              int err_len)
{
    err[0] = '\0';
    if (action == SS_DOWNLOAD && st->have_server)
        return do_download(cfg, st, err, err_len);
    if (action == SS_UPLOAD && st->have_local)
        return do_upload(cfg, st, err, err_len);
    if (action == SS_IN_SYNC && st->have_local && st->have_server)
    {
        base_save(cfg, st->rom_id, st->local_hash, st->srv.hash, st->srv.id);
        return true;
    }
    snprintf(err, err_len, "nothing to do");
    return false;
}

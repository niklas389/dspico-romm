// SPDX-License-Identifier: MIT
//
// dspico-romm proof of concept.
//
// Verifies on real DSi hardware, launched from Pico Launcher:
//   1. DSi mode + WPA2 Wi-Fi via DSWiFi
//   2. TLS 1.3 to the RomM server with Mbed TLS (handshake time, cipher)
//   3. RomM 5.4 device pairing (QR code on the top screen)
//   4. Paginated platform / ROM listing (JSON size per page)
//   5. Download throughput, network-only and network + SD write
//
// All results are printed on the bottom screen and written to romm/poc.log.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <dswifi9.h>
#include <fat.h>
#include <nds/arm9/dldi.h>
#include <nds.h>

#include "bench.h"
#include "download.h"
#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "prof.h"
#include "qr.h"
#include "romindex.h"
#include "unzipbench.h"

#define LIST_ROWS     16

static config_t g_cfg;

// One log per build variant, e.g. poc-32x1460-rx128.log
#define LOG_NAME "poc-" TCP_WND_LABEL ".log"
#define LOG_KEEP 5 // This run's log and the 4 before it (.1.log = previous)

// "x.log" -> "x.<n>.log"
static void log_name_n(const char *path, int n, char *out, int len)
{
    int stem = (int)strlen(path) - 4;
    if (n == 0)
        snprintf(out, len, "%s", path);
    else
        snprintf(out, len, "%.*s.%d.log", stem, path, n);
}

static void rotate_logs(const char *path)
{
    char from[CFG_PATH_MAX + 24], to[CFG_PATH_MAX + 24];
    log_name_n(path, LOG_KEEP - 1, to, sizeof(to));
    remove(to);
    for (int n = LOG_KEEP - 2; n >= 0; n--)
    {
        log_name_n(path, n, from, sizeof(from));
        log_name_n(path, n + 1, to, sizeof(to));
        rename(from, to);
    }
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------

static void frame(void)
{
    cothread_yield_irq(IRQ_VBLANK);
    scanKeys();
}

static u16 wait_key(void)
{
    while (1)
    {
        frame();
        u16 k = keysDown();
        if (k)
            return k;
    }
}

static void press_any_key(void)
{
    printf("\n(press any key)\n");
    wait_key();
}

static void fatal(const char *msg)
{
    log_printf("\nFATAL: %s\n", msg);
    log_printf("Log: %s" LOG_NAME "\nPress START to exit\n", g_cfg.base_dir);
    log_close();
    while (1)
    {
        frame();
        if (keysDown() & KEY_START)
            exit(1);
    }
}

// Waits ms milliseconds. Returns false if B was pressed.
static bool wait_ms_cancelable(uint32_t ms)
{
    uint32_t start = net_now_ms();
    while (net_now_ms() - start < ms)
    {
        frame();
        if (keysDown() & KEY_B)
            return false;
    }
    return true;
}

static void format_size(uint64_t bytes, char *out, size_t len)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, len, "%lu.%luM", (unsigned long)(bytes >> 20),
                 (unsigned long)(((bytes & 0xFFFFF) * 10) >> 20));
    else
        snprintf(out, len, "%luK", (unsigned long)(bytes >> 10));
}

#define MENU_BACK   -1
#define MENU_X      -4
#define MENU_SELECT -5
#define MENU_Y      -6

// Scrolling list. Returns the selected index or MENU_* for B/X/Y/SELECT.
// L/R move a page. hint is the bottom line.
static int menu_ex(const char *title, const char **items, int count, int *cursor,
                   const char *hint)
{
    int top = 0;
    while (1)
    {
        if (*cursor >= count)
            *cursor = count ? count - 1 : 0;
        if (*cursor < top)
            top = *cursor;
        if (*cursor >= top + LIST_ROWS)
            top = *cursor - LIST_ROWS + 1;

        consoleClear();
        printf("%.32s\n", title);
        printf("--------------------------------");
        for (int i = top; i < count && i < top + LIST_ROWS; i++)
            printf("%c%.30s\n", i == *cursor ? '>' : ' ', items[i]);
        printf("\x1b[21;0H%.32s", hint);

        u16 k = wait_key();
        if ((k & KEY_DOWN) && *cursor < count - 1)
            (*cursor)++;
        else if ((k & KEY_UP) && *cursor > 0)
            (*cursor)--;
        else if (k & KEY_R)
            *cursor = *cursor + LIST_ROWS < count ? *cursor + LIST_ROWS : count - 1;
        else if (k & KEY_L)
            *cursor = *cursor >= LIST_ROWS ? *cursor - LIST_ROWS : 0;
        else if ((k & KEY_A) && count > 0)
            return *cursor;
        else if (k & KEY_B)
            return MENU_BACK;
        else if (k & KEY_X)
            return MENU_X;
        else if (k & KEY_Y)
            return MENU_Y;
        else if (k & KEY_SELECT)
            return MENU_SELECT;
    }
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

static void step_system_info(void)
{
    log_printf("dspico-romm PoC %s\n", POC_VERSION);
    log_printf("DSi mode: %s\n", isDSiMode() ? "YES" : "NO (DS mode!)");
    log_printf("DLDI on: %s\n", dldiGetMode() == DLDI_MODE_ARM7 ? "ARM7" : "ARM9");

    if (isDSiMode())
    {
        setCpuClock(true);
        log_printf("CPU: 134 MHz\n");
    }
    else
    {
        log_printf("WARNING: no WPA2, 4 MB RAM.\n");
    }

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    log_printf("RTC: %s\n", ts);
    log_printf("(wrong clock breaks cert check)\n");
}

static void step_wifi(void)
{
    log_printf("Wi-Fi: connecting (WFC)...\n");

    if (!Wifi_InitDefault(WFC_CONNECT | WIFI_ATTEMPT_DSI_MODE))
        fatal("Wi-Fi connect failed.\n"
              "Set up the AP in DSi System\n"
              "Settings (slot 4-6 for WPA2).");

    u32 ip = Wifi_GetIP();
    unsigned char mac[6] = { 0 };
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);

    log_printf("Wi-Fi: IP %lu.%lu.%lu.%lu\n", (unsigned long)(ip & 0xFF),
               (unsigned long)((ip >> 8) & 0xFF),
               (unsigned long)((ip >> 16) & 0xFF),
               (unsigned long)((ip >> 24) & 0xFF));
    log_printf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2],
               mac[3], mac[4], mac[5]);

#ifdef DSPICO_DSWIFI
    // DSWiFi defaults to 1300, so lwIP advertises an MSS of 1260 and every
    // segment carries 1248 data bytes. 1500 means ~16 % fewer packets for the
    // ARM7, the SDIO bus and lwIP. Needs the larger ARM7 mailbox buffer from
    // patches/dswifi-fast-rx.patch.
    extern void dswifi_set_mtu(u16 mtu);
    dswifi_set_mtu(g_cfg.mtu);
    log_printf("MTU: %d\n", g_cfg.mtu);

    // Lets lwIP keep received frames in the shared ring instead of copying
    // them (patches/dswifi-zero-copy-rx.patch).
    extern bool dswifi_rx_zero_copy;
    dswifi_rx_zero_copy = g_cfg.zerocopy;
    log_printf("Zero-copy RX: %s\n", g_cfg.zerocopy ? "on" : "off");
#endif
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void step_heartbeat(void)
{
    buf_t body = { 0 };
    net_result_t res;

    log_printf("\n[1] GET /api/heartbeat (new conn)\n");
    bool ok = net_get("/api/heartbeat", false, &body, &res);
    net_log_result("heartbeat", &res);
    if (!ok || res.status != 200)
    {
        buf_free(&body);
        fatal("Heartbeat failed. See log\n(set verbose=1 for TLS trace)");
    }

    cJSON *root = cJSON_Parse(body.data);
    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "SYSTEM");
    const char *ver = json_str(sys, "VERSION");
    log_printf("RomM version: %s\n", ver ? ver : "?");
    cJSON_Delete(root);
    buf_free(&body);

    // Second request on the same handle should reuse the TLS connection.
    log_printf("[2] GET /api/heartbeat (reuse)\n");
    ok = net_get("/api/heartbeat", false, &body, &res);
    net_log_result("heartbeat2", &res);
    buf_free(&body);
}

static bool token_valid(void)
{
    if (g_cfg.token[0] == '\0')
        return false;

    buf_t body = { 0 };
    net_result_t res;
    bool ok = net_get("/api/users/me", true, &body, &res);

    if (ok && res.status == 200)
    {
        cJSON *root = cJSON_Parse(body.data);
        const char *user = json_str(root, "username");
        log_printf("Logged in as: %s\n", user ? user : "?");
        cJSON_Delete(root);
        buf_free(&body);
        return true;
    }

    log_printf("Token rejected (HTTP %ld)\n", res.status);
    buf_free(&body);
    return false;
}

static void step_pair(void)
{
    unsigned char mac[6] = { 0 };
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);
    char ident[32];
    snprintf(ident, sizeof(ident), "dsi-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "client_device_identifier", ident);
    cJSON_AddStringToObject(req, "name", "DSi (DSPico)");
    cJSON_AddStringToObject(req, "client", "dspico-romm");
    cJSON_AddStringToObject(req, "platform", "nds");
    cJSON_AddStringToObject(req, "client_version", POC_VERSION);
    const char *scopes[] = { "me.read", "roms.read", "platforms.read",
                             "assets.read", "assets.write", "devices.read",
                             "devices.write", "roms.user.read" };
    cJSON *arr = cJSON_AddArrayToObject(req, "requested_scopes");
    for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(scopes[i]));
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    buf_t body = { 0 };
    net_result_t res;
    log_printf("\n[3] Device pairing\n");
    bool ok = net_post_json("/api/auth/device/init", json, false, &body, &res);
    free(json);
    if (!ok || (res.status != 200 && res.status != 201))
    {
        net_log_result("pair init", &res);
        if (body.data)
            log_printf("%.200s\n", body.data);
        buf_free(&body);
        fatal("Pairing init failed");
    }

    cJSON *root = cJSON_Parse(body.data);
    buf_free(&body);
    const char *device_code = json_str(root, "device_code");
    const char *user_code = json_str(root, "user_code");
    const char *vpath = json_str(root, "verification_path_complete");
    int interval = (int)json_num(root, "interval", 5);
    int expires_in = (int)json_num(root, "expires_in", 600);

    if (!device_code || !user_code || !vpath)
    {
        cJSON_Delete(root);
        fatal("Unexpected pairing response");
    }

    char device_code_copy[160];
    snprintf(device_code_copy, sizeof(device_code_copy), "%s", device_code);

    char url[CFG_URL_MAX + 128];
    snprintf(url, sizeof(url), "%s%s", g_cfg.url, vpath);

    consoleClear();
    printf("Scan the QR code on the top\nscreen and approve the device.\n\n");
    printf("Or open:\n%s/pair/device\nand enter code:\n\n   %s\n\n", g_cfg.url,
           user_code);
    printf("B: cancel\n");
    log_file_only("Pair URL: %s\n", url);
    cJSON_Delete(root);

    if (!top_show_qr(url))
        printf("(URL too long for QR)\n");

    char poll[256];
    snprintf(poll, sizeof(poll), "{\"device_code\":\"%s\"}", device_code_copy);

    uint32_t deadline = net_now_ms() + (uint32_t)expires_in * 1000;
    while ((int32_t)(deadline - net_now_ms()) > 0)
    {
        if (!wait_ms_cancelable((uint32_t)interval * 1000))
        {
            top_clear();
            fatal("Pairing cancelled");
        }

        ok = net_post_json("/api/auth/device/token", poll, false, &body, &res);
        if (ok && res.status == 200)
        {
            cJSON *tok = cJSON_Parse(body.data);
            const char *access = json_str(tok, "access_token");
            if (access)
            {
                snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", access);
                if (!config_save_token(&g_cfg, g_cfg.token))
                    log_printf("WARNING: could not save token.txt\n");
            }
            cJSON_Delete(tok);
            buf_free(&body);
            top_clear();
            consoleClear();
            if (!g_cfg.token[0])
                fatal("Token response without access_token");
            log_printf("Paired. Token saved.\n");
            return;
        }

        const char *detail = NULL;
        cJSON *err = body.data ? cJSON_Parse(body.data) : NULL;
        if (err)
            detail = json_str(err, "detail");

        if (detail && strcmp(detail, "slow_down") == 0)
        {
            interval += 5;
        }
        else if (!detail || strcmp(detail, "authorization_pending") != 0)
        {
            log_printf("Pairing failed: HTTP %ld %s\n", res.status,
                       detail ? detail : res.error);
            cJSON_Delete(err);
            buf_free(&body);
            top_clear();
            fatal("Pairing failed");
        }
        cJSON_Delete(err);
        buf_free(&body);
    }

    top_clear();
    fatal("Pairing code expired");
}

// ---------------------------------------------------------------------------
// Library browsing and download test
// ---------------------------------------------------------------------------

typedef struct
{
    uint32_t last_ui_ms;
    uint64_t base;        // Bytes done before the current request (resume)
} dl_ui_t;

static bool download_progress(void *ctx, uint64_t now, uint64_t total,
                              uint32_t elapsed_ms)
{
    dl_ui_t *ui = ctx;
    (void)total;

    scanKeys();
    if (keysDown() & KEY_B)
        return false;

    if (elapsed_ms - ui->last_ui_ms < 500)
        return true;
    ui->last_ui_ms = elapsed_ms;

    uint32_t kbps = elapsed_ms ? (uint32_t)((now * 1000 / 1024) / elapsed_ms) : 0;
    char a[16];
    format_size(now, a, sizeof(a));
    printf("\x1b[8;0H%s  %lu KB/s  %lus        \n", a, (unsigned long)kbps,
           (unsigned long)(elapsed_ms / 1000));
    return true;
}

typedef struct
{
    uint32_t t0, last_ui_ms;
} unzip_ui_t;

static bool extract_progress(void *ctx, uint64_t done, uint64_t total)
{
    unzip_ui_t *ui = ctx;
    scanKeys();
    if (keysDown() & KEY_B)
        return false;
    uint32_t ms = net_now_ms() - ui->t0;
    if (ms - ui->last_ui_ms < 500)
        return true;
    ui->last_ui_ms = ms;
    uint32_t kbps = ms ? (uint32_t)((done * 1000 / 1024) / ms) : 0;
    printf("\x1b[12;0H%lu%%  %lu KB/s  %lus        \n",
           (unsigned long)(total ? done * 100 / total : 0), (unsigned long)kbps,
           (unsigned long)(ms / 1000));
    return true;
}

static void log_io(const sdraw_stats_t *io)
{
    if (!io->irqs)
        return;
    log_printf("  SD engine: IRQ %lu ms, %lu IRQs, %lu polls, ready after ~%lu us read / %lu us write\n",
               (unsigned long)(io->irq_ticks * 1000 / BUS_CLOCK), (unsigned long)io->irqs,
               (unsigned long)io->polls,
               (unsigned long)((uint64_t)io->avg_read_ticks * 1000000 / BUS_CLOCK),
               (unsigned long)((uint64_t)io->avg_write_ticks * 1000000 / BUS_CLOCK));
}

static void log_extract(const extract_result_t *x)
{
    uint32_t kbps = x->ms ? (uint32_t)((x->st.out_bytes * 1000 / 1024) / x->ms) : 0;
    log_printf("\n[Unpack] %s -> %s\n", x->entry, x->path);
    log_printf("  %s: %llu -> %llu B in %lu ms (%lu KB/s out), %s, %d file(s)\n",
               x->ok ? "done" : (x->cancelled ? "cancelled" : "FAILED"),
               (unsigned long long)x->st.in_bytes, (unsigned long long)x->st.out_bytes,
               (unsigned long)x->ms, (unsigned long)kbps,
               x->raw ? (x->raw_read ? "DSpico DMA read+write" : "DSpico DMA write")
                      : "DLDI", x->entries);
    log_printf("  read %lu ms, inflate %lu ms, crc %lu ms, SD wait %lu ms\n",
               (unsigned long)(x->st.read_us / 1000), (unsigned long)(x->st.inflate_us / 1000),
               (unsigned long)(x->st.crc_us / 1000), (unsigned long)(x->st.sink_us / 1000));
    log_io(&x->io);
    if (!x->ok && !x->cancelled)
        log_printf("  %s%s\n", x->error, x->zip_removed ? " (archive deleted)" : "");
}

// Downloads a ROM into <drive>/roms/<folder>/ (download.c). Archives are
// extracted afterwards.
static void download_to_library(int rom_id, const char *fs_name, uint64_t size,
                                const char *folder)
{
    bool archive = download_is_archive(fs_name);
    char sz[16];
    format_size(size, sz, sizeof(sz));

    consoleClear();
    printf("Download\n%.31s\n%s -> roms/%s/\n", fs_name, sz, folder);

    uint64_t have = 0;
    char local[256];
    if (download_local(&g_cfg, folder, fs_name, local, sizeof(local), &have))
    {
        char hs[16];
        format_size(have, hs, sizeof(hs));
        printf("\nAlready on SD (%s):\n%.31s\nA: download again  B: back\n", hs, local);
        if (!(wait_key() & KEY_A))
            return;
        consoleClear();
        printf("Download\n%.31s\n%s -> roms/%s/\n", fs_name, sz, folder);
    }
    printf("\nB: cancel\n");

    // An archive left by a failed extraction is reused
    uint64_t zsize = 0;
    bool have_zip = archive && download_exists(&g_cfg, folder, fs_name, &zsize) &&
                    zsize == size;

    // Download and extraction must fit: archive + ROM inside at the peak
    printf("\x1b[6;0HChecking free space...\n");
    space_check_t sc;
    bool fits = download_space_check(&g_cfg, rom_id, folder, fs_name, size, have_zip, &sc);
    char need_s[16], free_s[16], rom_s[16];
    format_size(sc.need, need_s, sizeof(need_s));
    format_size(sc.free, free_s, sizeof(free_s));
    format_size(sc.rom_size, rom_s, sizeof(rom_s));
    log_printf("\n[Space] need %s%s%s, free %s%s%s\n", need_s,
               sc.rom_size ? " incl. unpacked ROM " : "", sc.rom_size ? rom_s : "",
               free_s, sc.note[0] ? "; " : "", sc.note);
    printf("\x1b[6;0H                         \n");
    if (!fits)
    {
        printf("\x1b[10;0HNot enough space on the SD card:\nneeds %s, %s free.\n",
               need_s, free_s);
        press_any_key();
        return;
    }

    if (have_zip)
        log_printf("\n[Download] archive already on SD, unpacking it\n");
    else
    {
        dl_ui_t ui = { 0 };
        download_result_t r;
        bool ok = download_rom(&g_cfg, rom_id, fs_name, size, folder, download_progress,
                               &ui, &r);

        uint32_t kbps = r.ms ? (uint32_t)((r.bytes * 1000 / 1024) / r.ms) : 0;
        printf("\x1b[10;0H");
        log_printf("\n[Download] %s -> %s\n", fs_name, r.path);
        log_printf("  %s: %llu B in %lu ms (%lu KB/s), %s, %d request(s)\n",
                   ok ? "done" : (r.cancelled ? "cancelled" : "FAILED"),
                   (unsigned long long)r.bytes, (unsigned long)r.ms, (unsigned long)kbps,
                   r.raw ? "DSpico DMA" : "DLDI", r.attempts);
        log_io(&r.io);
        if (!ok && !r.cancelled)
            log_printf("  %s\n", r.error);
        if (!ok)
        {
            press_any_key();
            return;
        }
    }

    if (archive)
    {
        printf("\x1b[11;0HUnpacking...  B: cancel\n");
        unzip_ui_t ui = { net_now_ms(), 0 };
        extract_result_t x;
        download_extract(&g_cfg, folder, fs_name, extract_progress, &ui, &x);
        printf("\x1b[13;0H");
        log_extract(&x);
    }
    press_any_key();
}

// --- Library views --------------------------------------------------------

// Names of the files in <drive>/roms/<folder>/, sorted (for "on SD" marks).
typedef struct
{
    char **names;
    int count;
} dir_list_t;

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void dir_list_load(const char *folder, dir_list_t *d)
{
    memset(d, 0, sizeof(*d));
    char path[CFG_PATH_MAX + 64];
    download_target(&g_cfg, folder, "", path, sizeof(path));
    DIR *dir = opendir(path);
    if (!dir)
        return;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;
        if (d->count == cap)
        {
            cap = cap ? cap * 2 : 64;
            char **n = realloc(d->names, cap * sizeof(char *));
            if (!n)
                break;
            d->names = n;
        }
        d->names[d->count] = strdup(de->d_name);
        if (d->names[d->count])
            d->count++;
    }
    closedir(dir);
    qsort(d->names, d->count, sizeof(char *), cmp_str);
}

static bool dir_list_has(const dir_list_t *d, const char *name)
{
    return d->count &&
           bsearch(&name, d->names, d->count, sizeof(char *), cmp_str) != NULL;
}

static void dir_list_free(dir_list_t *d)
{
    for (int i = 0; i < d->count; i++)
        free(d->names[i]);
    free(d->names);
    memset(d, 0, sizeof(*d));
}

static void sync_progress(int fetched, int total)
{
    printf("\x1b[3;0H%d / %d ROMs        \n", fetched, total);
}

// Detail view of one ROM: metadata from the index, A downloads.
static void rom_detail(const rom_entry_t *e, const char *folder, bool *on_sd)
{
    while (1)
    {
        consoleClear();
        char sz[16];
        format_size(e->size, sz, sizeof(sz));
        printf("%.96s\n\n", e->name);
        if (e->year)
            printf("Year:    %d\n", e->year);
        if (e->genres[0])
            printf("Genre:   %.23s\n", e->genres);
        if (e->players[0])
            printf("Players: %.23s\n", e->players);
        if (e->regions[0])
            printf("Region:  %.23s\n", e->regions);
        printf("Size:    %s (.%s)\n", sz, e->ext);

        static char meta[2048];
        if (idx_meta(&g_cfg, e->id, meta, sizeof(meta)))
        {
            char *pub = strstr(meta, "publisher=");
            if (pub)
                printf("Pub.:    %.23s\n", strtok(pub + 10, "\n"));
            char *summary = strstr(pub ? pub + strlen(pub) + 1 : meta, "\n\n");
            if (summary)
                printf("\n%.160s\n", summary + 2);
        }

        printf("\x1b[19;0H%s\n", *on_sd ? "On SD card" : "");
        if (download_is_archive(e->fs_name))
            printf("(zip: unpacked after download)\n");
        else if (strcasecmp(e->ext, "7z") == 0 || strcasecmp(e->ext, "rar") == 0)
            printf("(archive: not launchable)\n");
        printf("\x1b[21;0HA:download Y:speed test B:back");
        if (*on_sd)
            printf("\x1b[22;0HX:unzip CPU test (ROM on SD)");

        u16 k = wait_key();
        if (k & KEY_B)
            return;
        if (k & KEY_A)
        {
            download_to_library(e->id, e->fs_name, e->size, folder);
            *on_sd = download_local(&g_cfg, folder, e->fs_name, NULL, 0, NULL);
        }
        else if (k & KEY_Y)
        {
            bench_run_suite(&g_cfg, e->id, e->fs_name, e->size);
            press_any_key();
        }
        else if ((k & KEY_X) && *on_sd)
        {
            char name[256], path[CFG_PATH_MAX + 256];
            if (download_local(&g_cfg, folder, e->fs_name, name, sizeof(name), NULL))
            {
                consoleClear();
                printf("Unzip CPU test\n%.31s\n\n", name);
                download_target(&g_cfg, folder, name, path, sizeof(path));
                unzip_bench(path);
                press_any_key();
            }
        }
    }
}

static void browse_platform(int platform_id, const char *platform_name,
                            const char *folder)
{
    rom_index_t idx;
    idx_init(&idx, platform_id);
    bool have = idx_load(&g_cfg, &idx);

    bool full = false;
    while (1)
    {
        // Bring the index up to date (incremental unless it is new or X)
        if (full)
        {
            idx_free(&idx);
            idx_init(&idx, platform_id);
        }
        consoleClear();
        printf("%.24s\n%s index...\n", platform_name,
               idx.count ? "Updating" : "Downloading");
        idx_sync_result_t r;
        bool ok = idx_sync(&g_cfg, &idx, sync_progress, &r);
        log_printf("\n[Index %s] %s: %d fetched, %d removed, %d ROMs, %d req, %lu ms\n",
                   platform_name, ok ? (r.full ? "full" : "update") : "FAILED",
                   r.fetched, r.removed, idx.count, r.requests, (unsigned long)r.ms);
        if (!ok)
        {
            log_printf("  %s\n", r.error);
            if (!have && idx.count == 0)
            {
                press_any_key();
                idx_free(&idx);
                return;
            }
            log_printf("  showing the saved index\n");
            press_any_key();
        }
        full = false;

        if (idx.count == 0)
        {
            printf("No ROMs.\n");
            press_any_key();
            idx_free(&idx);
            return;
        }

        dir_list_t onsd;
        dir_list_load(folder, &onsd);

        const char **labels = calloc(idx.count, sizeof(char *));
        char (*label_buf)[40] = calloc(idx.count, sizeof(*label_buf));
        bool *on_sd = calloc(idx.count, sizeof(bool));
        if (!labels || !label_buf || !on_sd)
        {
            free(labels);
            free(label_buf);
            free(on_sd);
            dir_list_free(&onsd);
            idx_free(&idx);
            return;
        }
        for (int i = 0; i < idx.count; i++)
        {
            const rom_entry_t *e = &idx.e[i];
            char cand[256];
            for (int k = 0; !on_sd[i] &&
                            download_local_name(folder, e->fs_name, k, cand, sizeof(cand));
                 k++)
                on_sd[i] = dir_list_has(&onsd, cand);
            char sz[16];
            format_size(e->size, sz, sizeof(sz));
            snprintf(label_buf[i], sizeof(label_buf[i]), "%c%-22.22s %6s",
                     on_sd[i] ? '*' : ' ', e->name, sz);
            labels[i] = label_buf[i];
        }
        dir_list_free(&onsd);

        char title[48];
        snprintf(title, sizeof(title), "%.20s (%d)", platform_name, idx.count);
        int cursor = 0;
        bool leave = false;
        while (!leave)
        {
            int sel = menu_ex(title, labels, idx.count, &cursor,
                              "A:open X:resync B:back *:on SD");
            if (sel == MENU_BACK)
                leave = true;
            else if (sel == MENU_X)
            {
                full = true;
                break;
            }
            else if (sel >= 0)
            {
                rom_detail(&idx.e[sel], folder, &on_sd[sel]);
                label_buf[sel][0] = on_sd[sel] ? '*' : ' ';
            }
        }
        free(labels);
        free(label_buf);
        free(on_sd);
        if (leave)
            break;
    }
    idx_free(&idx);
}

// Hidden platforms: <base>/hidden_platforms.txt, one platform id per line.
static bool hidden_load(int *ids, int max, int *n)
{
    *n = 0;
    char path[CFG_PATH_MAX + 32];
    config_path(&g_cfg, "hidden_platforms.txt", path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char line[32];
    while (*n < max && fgets(line, sizeof(line), f))
        if (atoi(line) > 0)
            ids[(*n)++] = atoi(line);
    fclose(f);
    return true;
}

static void hidden_save(const int *ids, int n)
{
    char path[CFG_PATH_MAX + 32];
    config_path(&g_cfg, "hidden_platforms.txt", path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%d\n", ids[i]);
    fclose(f);
}

static void step_platforms(void)
{
    buf_t body = { 0 };
    net_result_t res;

    log_printf("\n[4] GET /api/platforms\n");
    bool ok = net_get("/api/platforms", true, &body, &res);
    net_log_result("platforms", &res);
    if (!ok || res.status != 200)
    {
        buf_free(&body);
        fatal("Platform list failed");
    }

    cJSON *root = cJSON_Parse(body.data);
    buf_free(&body);
    int count = cJSON_GetArraySize(root);
    log_printf("%d platforms\n", count);
    if (count <= 0)
    {
        cJSON_Delete(root);
        return;
    }

    typedef struct { int id, roms; char name[28]; char folder[32]; } plat_t;
    plat_t *pl = calloc(count, sizeof(plat_t));
    for (int i = 0; i < count; i++)
    {
        const cJSON *p = cJSON_GetArrayItem(root, i);
        const char *name = json_str(p, "display_name");
        if (!name)
            name = json_str(p, "name");
        pl[i].id = (int)json_num(p, "id", 0);
        pl[i].roms = (int)json_num(p, "rom_count", 0);
        snprintf(pl[i].name, sizeof(pl[i].name), "%s", name ? name : "?");
        snprintf(pl[i].folder, sizeof(pl[i].folder), "%s",
                 download_folder(json_str(p, "slug"), json_str(p, "fs_slug")));
    }
    cJSON_Delete(root);

    int hidden[64], n_hidden = 0;
    hidden_load(hidden, 64, &n_hidden);
    bool show_all = false;

    const char **labels = calloc(count, sizeof(char *));
    char (*label_buf)[40] = calloc(count, sizeof(*label_buf));
    int *map = calloc(count, sizeof(int));
    int cursor = 0;
    while (1)
    {
        int n = 0;
        for (int i = 0; i < count; i++)
        {
            bool hid = false;
            for (int j = 0; j < n_hidden; j++)
                hid |= hidden[j] == pl[i].id;
            if (hid && !show_all)
                continue;
            snprintf(label_buf[n], sizeof(label_buf[n]), "%c%-24.24s %5d",
                     hid ? '-' : ' ', pl[i].name, pl[i].roms);
            labels[n] = label_buf[n];
            map[n++] = i;
        }
        int sel = menu_ex(show_all ? "Platforms (all)" : "Platforms", labels, n, &cursor,
                          show_all ? "A:open SEL:hide/show Y:done" : "A:open Y:edit B:quit");
        if (sel == MENU_BACK)
            break;
        if (sel == MENU_Y)
        {
            show_all = !show_all;
            continue;
        }
        if (sel == MENU_SELECT && show_all && n > 0)
        {
            int id = pl[map[cursor]].id, j;
            for (j = 0; j < n_hidden && hidden[j] != id; j++)
                ;
            if (j < n_hidden)
                hidden[j] = hidden[--n_hidden];
            else if (n_hidden < 64)
                hidden[n_hidden++] = id;
            hidden_save(hidden, n_hidden);
            continue;
        }
        if (sel >= 0)
        {
            const plat_t *p = &pl[map[sel]];
            browse_platform(p->id, p->name, p->folder);
        }
    }

    free(labels);
    free(label_buf);
    free(map);
    free(pl);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    defaultExceptionHandler();

    top_init();
    consoleDemoInit(); // Bottom screen text console

#ifdef DLDI_FORCE_ARM9
    // Run the SD driver on the ARM9 so the ARM7 is free for Wi-Fi.
    dldiSetMode(DLDI_MODE_ARM9);
#endif
    if (!fatInitDefault())
    {
        printf("FAT init failed\n");
        fatal("No SD access");
    }

    bool have_cfg = config_load(&g_cfg);
    if (g_cfg.base_dir[0])
    {
        char path[CFG_PATH_MAX + 24];
        config_path(&g_cfg, LOG_NAME, path, sizeof(path));
        rotate_logs(path);
        log_open(path);
    }

    step_system_info();

    extern bool selftest_chachapoly(void);
    if (!selftest_chachapoly())
        fatal("ChaCha20-Poly1305 self-test FAILED");
    log_printf("Crypto self-test: OK\n");

    if (!have_cfg)
        fatal("romm/config.ini not found or\nmissing url= (see README)");

    log_printf("Config: %sconfig.ini\n", g_cfg.base_dir);
    log_printf("Save slot: %s\n", g_cfg.save_slot);
    log_file_only("URL: %s insecure=%d verbose=%d\n", g_cfg.url, g_cfg.insecure,
                  g_cfg.verbose);

    step_wifi();
    prof_init();

    if (!net_init(&g_cfg))
        fatal("Network init failed");

    if (g_cfg.insecure)
        log_printf("WARNING: cert check disabled\n");

    step_heartbeat();
    press_any_key();

    if (!token_valid())
    {
        config_delete_token(&g_cfg);
        g_cfg.token[0] = '\0';
        step_pair();
        if (!token_valid())
            fatal("New token does not work");
    }

    step_platforms();

    log_printf("\nDone. Log: %s" LOG_NAME "\n", g_cfg.base_dir);
    net_cleanup();
    Wifi_DisconnectAP();
    Wifi_DisableWifi();
    log_close();

    printf("Press START to exit\n");
    while (1)
    {
        frame();
        if (keysDown() & KEY_START)
            break;
    }
    return 0;
}

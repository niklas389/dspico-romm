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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <dswifi9.h>
#include <fat.h>
#include <nds.h>

#include "bench.h"
#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "qr.h"

#define ROMS_PER_PAGE 10
#define LIST_ROWS     16

static config_t g_cfg;

// One log per build variant, e.g. poc-32x1460-rx128.log
#define LOG_NAME "poc-" TCP_WND_LABEL ".log"

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

// Returns the selected index, or -1 on B. L/R return -2/-3 for paging.
static int menu(const char *title, const char **items, int count, int *cursor)
{
    int top = 0;
    while (1)
    {
        if (*cursor < top)
            top = *cursor;
        if (*cursor >= top + LIST_ROWS)
            top = *cursor - LIST_ROWS + 1;

        consoleClear();
        printf("%s\n", title);
        printf("--------------------------------");
        for (int i = top; i < count && i < top + LIST_ROWS; i++)
            printf("%c%.30s\n", i == *cursor ? '>' : ' ', items[i]);
        printf("\x1b[21;0HA:select B:back L/R:page");

        u16 k = wait_key();
        if ((k & KEY_DOWN) && *cursor < count - 1)
            (*cursor)++;
        else if ((k & KEY_UP) && *cursor > 0)
            (*cursor)--;
        else if (k & KEY_A)
            return *cursor;
        else if (k & KEY_B)
            return -1;
        else if (k & KEY_L)
            return -2;
        else if (k & KEY_R)
            return -3;
    }
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

static void step_system_info(void)
{
    log_printf("dspico-romm PoC %s\n", POC_VERSION);
    log_printf("DSi mode: %s\n", isDSiMode() ? "YES" : "NO (DS mode!)");

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
    const char *label;
    uint32_t last_ui_ms;
    bool cancelled;
} dl_ui_t;

static bool download_progress(void *ctx, uint64_t now, uint64_t total,
                              uint32_t elapsed_ms)
{
    dl_ui_t *ui = ctx;

    scanKeys();
    if (keysDown() & KEY_B)
    {
        ui->cancelled = true;
        return false;
    }

    if (elapsed_ms - ui->last_ui_ms < 500)
        return true;
    ui->last_ui_ms = elapsed_ms;

    uint32_t kbps = elapsed_ms ? (uint32_t)((now * 1000 / 1024) / elapsed_ms) : 0;
    char a[16], b[16];
    format_size(now, a, sizeof(a));
    format_size(total, b, sizeof(b));
    printf("\x1b[10;0H%s / %s          \n%lu KB/s  %lus        \n", a, b,
           (unsigned long)kbps, (unsigned long)(elapsed_ms / 1000));
    return true;
}

static void download_test(int rom_id, const char *fs_name, uint64_t size,
                          bool to_sd)
{
    char *escaped = net_escape(fs_name);
    if (!escaped)
        return;

    char path[512];
    snprintf(path, sizeof(path), "/api/roms/%d/content/%s", rom_id, escaped);
    free(escaped);

    FILE *fp = NULL;
    char out_path[CFG_PATH_MAX + 256];
    if (to_sd)
    {
        char dir[CFG_PATH_MAX + 16];
        config_path(&g_cfg, "test", dir, sizeof(dir));
        mkdir(dir, 0777);
        snprintf(out_path, sizeof(out_path), "%s/%s", dir, fs_name);
        fp = fopen(out_path, "wb");
        if (!fp)
        {
            log_printf("Can't create %s\n", out_path);
            press_any_key();
            return;
        }
        // Large buffer so FAT writes happen in big sequential chunks.
        setvbuf(fp, NULL, _IOFBF, 64 * 1024);
    }

    consoleClear();
    char sz[16];
    format_size(size, sz, sizeof(sz));
    printf("Downloading (%s)\n%.31s\n%s\n\nB: cancel\n", to_sd ? "to SD" : "discard",
           fs_name, sz);

    dl_ui_t ui = { 0 };
    net_result_t res;
    net_wifi_stats_t st0, st1;
    net_wifi_stats(&st0);
    bool ok = net_download(path, fp, download_progress, &ui, &res);
    net_wifi_stats(&st1);

    uint32_t close_ms = 0;
    if (fp)
    {
        uint32_t t0 = net_now_ms();
        fclose(fp);
        close_ms = net_now_ms() - t0;
        if (!ok)
            remove(out_path);
    }

    printf("\x1b[13;0H");
    log_printf("\n[DL %s] %s\n", to_sd ? "SD" : "net", fs_name);
    net_log_result("download", &res);
    if (res.total_ms > 0)
    {
        uint32_t kbps = (uint32_t)((res.body_bytes * 1000 / 1024) / res.total_ms);
        log_printf("  => %lu KB/s avg\n", (unsigned long)kbps);
    }
    if (fp)
        log_printf("  fclose %lu ms\n", (unsigned long)close_ms);
    log_printf("  rx queued %lu, lost %lu\n",
               (unsigned long)(st1.rx_queued - st0.rx_queued),
               (unsigned long)(st1.rx_lost - st0.rx_lost));
    if (ui.cancelled)
        log_printf("  cancelled by user\n");
    else if (!ok)
        log_printf("  FAILED\n");

    press_any_key();
}

static void browse_platform(int platform_id, const char *platform_name)
{
    int offset = 0;
    int cursor = 0;

    while (1)
    {
        char path[256];
        snprintf(path, sizeof(path),
                 "/api/roms?platform_ids=%d&limit=%d&offset=%d"
                 "&order_by=name&order_dir=asc",
                 platform_id, ROMS_PER_PAGE, offset);

        consoleClear();
        printf("Loading...\n");

        buf_t body = { 0 };
        net_result_t res;
        bool ok = net_get(path, true, &body, &res);
        log_printf("\n[ROMs %s +%d]\n", platform_name, offset);
        net_log_result("roms page", &res);
        if (!ok || res.status != 200)
        {
            buf_free(&body);
            press_any_key();
            return;
        }

        uint32_t t0 = net_now_ms();
        cJSON *root = cJSON_Parse(body.data);
        log_printf("  JSON parse %lu ms, %u B\n",
                   (unsigned long)(net_now_ms() - t0), (unsigned)body.len);
        buf_free(&body);

        const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
        int total = (int)json_num(root, "total", 0);
        int count = cJSON_GetArraySize(items);

        if (count == 0)
        {
            cJSON_Delete(root);
            printf("No ROMs.\n");
            press_any_key();
            return;
        }

        const char *labels[ROMS_PER_PAGE];
        static char label_buf[ROMS_PER_PAGE][40];
        for (int i = 0; i < count && i < ROMS_PER_PAGE; i++)
        {
            const cJSON *it = cJSON_GetArrayItem(items, i);
            const char *name = json_str(it, "name");
            if (!name)
                name = json_str(it, "fs_name");
            char sz[16];
            format_size((uint64_t)json_num(it, "fs_size_bytes", 0), sz, sizeof(sz));
            snprintf(label_buf[i], sizeof(label_buf[i]), "%-23.23s %6s",
                     name ? name : "?", sz);
            labels[i] = label_buf[i];
        }

        char title[64];
        snprintf(title, sizeof(title), "%.18s %d-%d/%d", platform_name, offset + 1,
                 offset + count, total);

        if (cursor >= count)
            cursor = count - 1;
        int sel = menu(title, labels, count, &cursor);

        if (sel == -1)
        {
            cJSON_Delete(root);
            return;
        }
        if (sel == -2)
        {
            if (offset >= ROMS_PER_PAGE)
                offset -= ROMS_PER_PAGE;
            cJSON_Delete(root);
            continue;
        }
        if (sel == -3)
        {
            if (offset + ROMS_PER_PAGE < total)
                offset += ROMS_PER_PAGE;
            cJSON_Delete(root);
            continue;
        }

        const cJSON *it = cJSON_GetArrayItem(items, sel);
        int rom_id = (int)json_num(it, "id", 0);
        const char *fs_name = json_str(it, "fs_name");
        uint64_t size = (uint64_t)json_num(it, "fs_size_bytes", 0);
        char fs_name_copy[256];
        snprintf(fs_name_copy, sizeof(fs_name_copy), "%s", fs_name ? fs_name : "rom.bin");
        cJSON_Delete(root);

        consoleClear();
        printf("%s\n\nA: download to SD\nX: download, discard (net only)\n"
               "Y: speed test suite\nB: back\n",
               label_buf[sel]);
        u16 k = wait_key();
        if (k & KEY_A)
            download_test(rom_id, fs_name_copy, size, true);
        else if (k & KEY_X)
            download_test(rom_id, fs_name_copy, size, false);
        else if (k & KEY_Y)
        {
            bench_run_suite(&g_cfg, rom_id, fs_name_copy, size);
            press_any_key();
        }
    }
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
    press_any_key();

    if (count <= 0)
    {
        cJSON_Delete(root);
        return;
    }

    const char **labels = calloc(count, sizeof(char *));
    char (*label_buf)[40] = calloc(count, sizeof(*label_buf));
    int *ids = calloc(count, sizeof(int));

    for (int i = 0; i < count; i++)
    {
        const cJSON *p = cJSON_GetArrayItem(root, i);
        const char *name = json_str(p, "display_name");
        if (!name)
            name = json_str(p, "name");
        ids[i] = (int)json_num(p, "id", 0);
        snprintf(label_buf[i], sizeof(label_buf[i]), "%-24.24s %5d",
                 name ? name : "?", (int)json_num(p, "rom_count", 0));
        labels[i] = label_buf[i];
    }

    int cursor = 0;
    while (1)
    {
        int sel = menu("Platforms (B: quit)", labels, count, &cursor);
        if (sel == -1)
            break;
        if (sel < 0)
            continue;

        char name[32];
        snprintf(name, sizeof(name), "%.24s", label_buf[sel]);
        browse_platform(ids[sel], name);
    }

    free(labels);
    free(label_buf);
    free(ids);
    cJSON_Delete(root);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    defaultExceptionHandler();

    top_init();
    consoleDemoInit(); // Bottom screen text console

    if (!fatInitDefault())
    {
        printf("FAT init failed\n");
        fatal("No SD access");
    }

    bool have_cfg = config_load(&g_cfg);
    if (g_cfg.base_dir[0])
    {
        char path[CFG_PATH_MAX + 16];
        config_path(&g_cfg, LOG_NAME, path, sizeof(path));
        log_open(path);
    }

    step_system_info();

    if (!have_cfg)
        fatal("romm/config.ini not found or\nmissing url= (see README)");

    log_printf("Config: %sconfig.ini\n", g_cfg.base_dir);
    log_printf("Save slot: %s\n", g_cfg.save_slot);
    log_file_only("URL: %s insecure=%d verbose=%d\n", g_cfg.url, g_cfg.insecure,
                  g_cfg.verbose);

    step_wifi();

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

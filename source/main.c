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
#include "gfx.h"
#include "cJSON.h"
#include "config.h"
#include "covers.h"
#include "log.h"
#include "net.h"
#include "prof.h"
#include "qr.h"
#include "romindex.h"
#include "savesync.h"
#include "ui.h"
#include "unzipbench.h"

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
// UI helpers (screens: ui.c, drawing: gfx.c)
// ---------------------------------------------------------------------------

static bool g_quit;          // START: leave the app
static bool g_console;       // Bottom screen shows the text console

static bool g_wifi_up;
static bool g_top_header;    // Top screen has the status bar (clock, Wi-Fi)
static uint32_t g_frames;
static const char *clock_str(void);

// Wi-Fi icon level: -1 not connected, 1..3 bars from the RSSI value.
// DSWiFi documents 0..255 for WIFIGETDATA_RSSI; the thresholds are a first
// guess (logged at connect for calibration).
static int wifi_level(void)
{
    if (!g_wifi_up)
        return -2;
    if (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
        return -1;
    int r = Wifi_GetData(WIFIGETDATA_RSSI, 0, NULL);
    return r >= 180 ? 3 : r >= 140 ? 2 : 1; // 202 measured near the AP (-54 dBm)
}

static void frame(void)
{
    if (++g_frames % 60 == 0)
    {
        // Clock and Wi-Fi state once a second
        ui_wifi_level = wifi_level();
        if (g_frames % 600 == 60 || ui_battery < 0)
            ui_battery = (int)(getBatteryLevel() & 0x8F); // Every 10 s (asks the ARM7)
        if (g_top_header)
            ui_top_header(gfx_top, "RomM", clock_str());
    }
    gfx_present(); // Waits for VBlank (other threads run meanwhile)
    scanKeys();
}

static const char *clock_str(void)
{
    static char b[8];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(b, sizeof(b), "%02d:%02d", tm->tm_hour % 100, tm->tm_min % 100);
    return b;
}

static void format_size(uint64_t bytes, char *out, size_t len)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, len, "%lu.%lu MB", (unsigned long)(bytes >> 20),
                 (unsigned long)(((bytes & 0xFFFFF) * 10) >> 20));
    else
        snprintf(out, len, "%lu KB", (unsigned long)((bytes + 1023) >> 10));
}

#ifdef DSPICO_DSWIFI
static void wifi_fw_log(void);
#endif

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

// Message on the bottom screen; waits for A or B. Returns true for A.
static bool message(int kind, const char *title, const char *body, const char *hint)
{
    ui_draw_message(gfx_bot, kind, title, body, hint ? hint : "A: OK");
    while (1)
    {
        u16 k = wait_key();
        if (k & KEY_A)
            return true;
        if (k & KEY_B)
            return false;
    }
}

// <base>/ui.txt: interface settings changed in the app (dark=0/1)
static void ui_settings_load(void)
{
    char p[CFG_PATH_MAX + 16];
    config_path(&g_cfg, "ui.txt", p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f)
        return;
    char line[64];
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "dark=", 5))
        {
            ui_set_dark(line[5] == '1');
            cover_set_bg(ui_dark);
        }
    fclose(f);
}

static void ui_settings_save(void)
{
    char p[CFG_PATH_MAX + 16];
    config_path(&g_cfg, "ui.txt", p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f)
        return;
    fprintf(f, "dark=%d\n", ui_dark ? 1 : 0);
    fclose(f);
}

// START: asks before leaving the app (errors exit without asking)
static bool confirm_quit(void)
{
    return message(UI_INFO, "Quit RomM?", "Wi-Fi is switched off and the DSi returns to the "
                   "launcher.", "A: quit  B: cancel");
}

// Text console on the bottom screen (log view, speed tests)
static void console_begin(void)
{
    consoleDemoInit();
    consoleClear();
    log_set_echo(true);
    g_console = true;
}

static void console_end(void)
{
    log_set_echo(false);
    gfx_init_bottom();
    g_console = false;
}

static void console_wait(void)
{
    printf("\n(press any key)\n");
    wait_key();
}

static bool wifi_shutdown(void);

static void splash_top(const char *line)
{
    g_top_header = false;
    ui_draw_splash_top(gfx_top, "Version " POC_VERSION, line);
}

static void fatal(const char *msg)
{
    log_printf("\nFATAL: %s\n", msg);
    log_close();
    if (g_console)
        console_end();
    // First line: title, rest: body
    char title[64], body[400];
    const char *nl = strchr(msg, '\n');
    snprintf(title, sizeof(title), "%.*s", nl ? (int)(nl - msg) : 63, msg);
    snprintf(body, sizeof(body), "%s%sLog: %s" LOG_NAME, nl ? nl + 1 : "", nl ? "\n\n" : "",
             g_cfg.base_dir);
    for (char *p = body; *p; p++) // Single line breaks are wrapped text
        if (p[0] == '\n' && p[1] != '\n' && p > body && p[-1] != '\n')
            *p = ' ';
    ui_draw_message(gfx_bot, UI_ERROR, title, body, "START: exit");
    while (1)
    {
        frame();
        if (keysDown() & KEY_START)
        {
            wifi_shutdown();
            exit(1);
        }
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

// --- Start-up steps ----------------------------------------------------------

enum { ST_SD, ST_WIFI, ST_SERVER, ST_LOGIN, ST_SAVES, ST_LIBRARY, ST_COUNT };
static ui_step_t g_steps[ST_COUNT] = {
    { "SD card", 0, NULL },     { "Wi-Fi", 0, NULL },     { "RomM server", 0, NULL },
    { "Sign in", 0, NULL },     { "Save sync", 0, NULL }, { "Game library", 0, NULL },
};
static char g_step_detail[ST_COUNT][40];

static void step_mark(int i, int state, const char *detail)
{
    g_steps[i].state = state;
    if (detail)
    {
        snprintf(g_step_detail[i], sizeof(g_step_detail[i]), "%s", detail);
        g_steps[i].detail = g_step_detail[i];
    }
    if (!g_console)
    {
        ui_draw_steps(gfx_bot, "Starting", g_steps, ST_COUNT, NULL);
        frame();
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

// Disconnects and switches the Wi-Fi hardware off (the ARM7 shuts the card
// down; the next Wifi_AutoConnect() initializes it again from scratch).
// Wifi_Deinit() is not usable with lwIP in this DSWiFi version (it crashes:
// "DSWifi can't be disabled if lwIP is active").
static bool wifi_shutdown(void)
{
    if (!g_wifi_up)
        return true;
    Wifi_DisconnectAP();
    Wifi_DisableWifi();
    for (int i = 0; i < 60; i++) // Let the ARM7 reach "disabled"
        cothread_yield_irq(IRQ_VBLANK); // lets lwIP (DHCP) run
    return true;
}

#ifdef DSPICO_DSWIFI
extern void Wifi_A7SetOpts(u32 opts);
static u32 a7_opts_base(void)
{
    return g_cfg.wifireset ? 4u : 0u; // DSPICO_A7_OPT_HWRESET
}
#endif

#ifdef DSPICO_DSWIFI
// Connect diagnostics from the patched ARM7 core (DSPICO_A7D_* in DSWiFi's
// wifi_shared.h) plus the access points the scan found. Shows at which stage
// a failed start stopped: card init, firmware, channel list, scan, connect.
static void wifi_diag_log(void)
{
    extern void Wifi_A7Diag(u32 out[25]);
    extern void Wifi_A7Info(u32 out[16]);
    u32 d[25], a7[16];
    Wifi_A7Diag(d);
    Wifi_A7Info(a7);
    char wl[16];
    if (g_cfg.wifireset) // before > after off > after on
        snprintf(wl, sizeof(wl), "%02lX>%02lX>%02lX", (unsigned long)(d[1] & 0xFF),
                 (unsigned long)((d[1] >> 8) & 0xFF), (unsigned long)((d[1] >> 16) & 0xFF));
    else // before > after on
        snprintf(wl, sizeof(wl), "%02lX>%02lX", (unsigned long)(d[1] & 0xFF),
                 (unsigned long)((d[1] >> 16) & 0xFF));
    log_printf("  init: starts %lu, SCFG_EXT %08lX, SCFG_WL %s, CMD5 x%lu%s%s\n",
               (unsigned long)d[15], (unsigned long)d[0], wl, (unsigned long)(d[2] >> 8),
               (d[2] & 1) ? " skipped" : "", (d[2] & 2) ? " 1-bit" : "");
    log_printf("  fw: uploaded before %lX, reset cause %lX, ready timeouts %lu,"
               " HTC ready %lu B, WMI ready %lu, regdomain %lu, channels %lu\n",
               (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[18],
               (unsigned long)a7[1], (unsigned long)d[5], (unsigned long)d[6],
               (unsigned long)d[7]);
    log_printf("  watchdog: %lu card re-inits (%lu with SDIO reset), %lu IRQs found"
               " by polling (last status %08lX), WMI ready after %lu ticks (~ms)\n",
               (unsigned long)(d[16] & 0xFF), (unsigned long)(d[16] >> 8),
               (unsigned long)d[17], (unsigned long)d[20], (unsigned long)d[19]);
    log_printf("  init packets: %lu HTC, %lu WMI, %lu other, %lu bad;"
               " first header %08lX, last %08lX\n",
               (unsigned long)(d[21] & 0xFF), (unsigned long)((d[21] >> 8) & 0xFF),
               (unsigned long)((d[21] >> 16) & 0xFF), (unsigned long)(d[21] >> 24),
               (unsigned long)d[23], (unsigned long)d[22]);
    log_printf("  scan: %lu started, %lu done, %lu BSS (%lu low SNR), SNR max %lu"
               " last %lu, mode %lu\n",
               (unsigned long)d[8], (unsigned long)d[9], (unsigned long)d[10],
               (unsigned long)d[11], (unsigned long)(d[12] & 0xFF),
               (unsigned long)((d[12] >> 8) & 0xFF), (unsigned long)d[24]);
    log_printf("  conn: %lu connect, %lu disconnect (last reason %lu),"
               " cmd errors %lu, target error %lX\n",
               (unsigned long)(d[14] & 0xFF), (unsigned long)((d[14] >> 8) & 0xFF),
               (unsigned long)(d[14] >> 16), (unsigned long)(d[13] & 0xFFFF),
               (unsigned long)(d[13] >> 16));

    static Wifi_AccessPoint aps[16];
    int n = Wifi_GetNumAP(), k = 0;
    for (int i = 0; i < n && k < 16; i++)
        if (Wifi_GetAPData(i, &aps[k]) == WIFI_RETURN_OK)
            k++;
    log_printf("  APs: %d listed\n", n);
    for (int i = 0; i < k && i < 8; i++)
        log_printf("    ch %2u rssi %4d sec %u %.12s\n", aps[i].channel, aps[i].rssi, (unsigned)aps[i].security_type,
                   aps[i].ssid);
}
#endif

// Waits up to ~20 s for the association, logging each state change.
static bool wifi_wait_assoc(int attempt)
{
    int last = -1;
    for (int frames = 0; frames < 20 * 60; frames++)
    {
        int st = Wifi_AssocStatus();
        if (st != last)
        {
            log_printf("  %4d ms: %s\n", frames * 1000 / 60,
                       (st >= 0 && st <= ASSOCSTATUS_CANNOTCONNECT) ? ASSOCSTATUS_STRINGS[st]
                                                                    : "?");
            last = st;
        }
        if (st == ASSOCSTATUS_ASSOCIATED)
            return true;
        if (st == ASSOCSTATUS_CANNOTCONNECT)
            break;
        cothread_yield_irq(IRQ_VBLANK); // lets lwIP (DHCP) run
    }
#ifdef DSPICO_DSWIFI
    extern void Wifi_A7Info(u32 out[16]);
    u32 a7[16];
    Wifi_A7Info(a7);
    log_printf("Wi-Fi: attempt %d failed, BMI target 0x%08lX%s\n", attempt,
               (unsigned long)a7[0],
               a7[0] == 0xFFFFFFFF ? " (module not answering)"
               : a7[0] == 0 ? " (module not initialized)" : "");
    wifi_diag_log();
#else
    log_printf("Wi-Fi: attempt %d failed\n", attempt);
#endif
    return false;
}


static void step_wifi(void)
{
    log_printf("Wi-Fi: connecting...\n");
    const char *msg = "Wi-Fi connect failed\n"
                      "Set up the access point in the DSi System Settings (slot 4-6 for "
                      "WPA2). If it worked before: switch the DSi off and on.";

    if (!Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE))
        fatal(msg);
    g_wifi_up = true;
#ifdef DSPICO_DSWIFI
    // Before the first AutoConnect: the ARM7 initializes the card then
    Wifi_A7SetOpts(a7_opts_base());
    log_printf("Wi-Fi: module reset %s, a7cmd53 %d, a7dma %d\n",
               g_cfg.wifireset ? "on" : "off", g_cfg.a7cmd53 ? 1 : 0, g_cfg.a7dma ? 1 : 0);
#endif
    log_sync();
    Wifi_AutoConnect();
    bool ok = wifi_wait_assoc(1);
    log_sync();

    if (!ok)
    {
        // 2: same library instance, new association attempt
        step_mark(ST_WIFI, 1, "retry 2 of 3");
        Wifi_DisconnectAP();
        for (int i = 0; i < 60; i++)
            cothread_yield_irq(IRQ_VBLANK); // lets lwIP (DHCP) run
        Wifi_AutoConnect();
        ok = wifi_wait_assoc(2);
    }
    if (!ok)
    {
        // 3: switch the hardware off and initialize the card again (with
        // the module reset if enabled)
        step_mark(ST_WIFI, 1, "retry 3 of 3");
        log_sync();
        wifi_shutdown();
        Wifi_AutoConnect();
        ok = wifi_wait_assoc(3);
    }
    if (!ok)
        fatal(msg);

    // Wifi_InitDefault() can return before DHCP has finished: wait for the
    // address (the ARM7 receive options below are only enabled afterwards).
    u32 ip = Wifi_GetIP();
    for (int i = 0; i < 600 && (ip == 0 || ip == 0xFFFFFFFF); i++) // ~10 s
    {
        cothread_yield_irq(IRQ_VBLANK); // lets lwIP (DHCP) run
        ip = Wifi_GetIP();
    }
    unsigned char mac[6] = { 0 };
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);

    log_printf("Wi-Fi: IP %lu.%lu.%lu.%lu\n", (unsigned long)(ip & 0xFF),
               (unsigned long)((ip >> 8) & 0xFF),
               (unsigned long)((ip >> 16) & 0xFF),
               (unsigned long)((ip >> 24) & 0xFF));
    log_printf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2],
               mac[3], mac[4], mac[5]);
    char ips[24];
    snprintf(ips, sizeof(ips), "%lu.%lu.%lu.%lu", (unsigned long)(ip & 0xFF),
             (unsigned long)((ip >> 8) & 0xFF), (unsigned long)((ip >> 16) & 0xFF),
             (unsigned long)((ip >> 24) & 0xFF));
    step_mark(ST_WIFI, 2, ips);
    log_printf("Wi-Fi RSSI value: %d\n", Wifi_GetData(WIFIGETDATA_RSSI, 0, NULL));

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

    // ARM7 receive options (patches/dswifi-arm7-rx-dma.patch), applied by
    // the ARM7 only while connected.
    extern void Wifi_A7SetOpts(u32 opts);
    Wifi_A7SetOpts(a7_opts_base() | (g_cfg.a7cmd53 ? 1u : 0u) | (g_cfg.a7dma ? 2u : 0u));
    log_printf("ARM7 RX: cmd53 %s, DMA to ring %s\n", g_cfg.a7cmd53 ? "on" : "off",
               g_cfg.a7dma ? "on" : "off");
    wifi_fw_log();
#endif
}

#ifdef DSPICO_DSWIFI
// Wi-Fi firmware facts the patched ARM7 core records (DSPICO_A7I_* in
// DSWiFi's wifi_shared.h): BMI target, HTC ready (credits, endpoints, HTC
// version and bundle size from HTC 2.1 on), WMI ready, SDIO settings.
static void wifi_fw_log(void)
{
    extern void Wifi_A7Info(u32 out[16]);
    u32 a[16];
    Wifi_A7Info(a);
    const u8 *h = (const u8 *)&a[2];
    log_printf("Wi-Fi FW: BMI target 0x%08lX, SDIO clk %lu bus %lu-bit blk %lu\n",
               (unsigned long)a[0], (unsigned long)(a[9] & 0xFF),
               (unsigned long)((a[9] >> 8) & 0xFF), (unsigned long)(a[9] >> 16));
    log_printf("  HTC ready %lu B: credits %u x %u B, max ep %u, ver %u, bundle %u\n",
               (unsigned long)a[1], h[0] | h[1] << 8, h[2] | h[3] << 8, h[4],
               a[1] >= 8 ? h[6] : 0, a[1] >= 8 ? h[7] : 0);
    log_printf("  WMI ready %lu B: %08lX %08lX %08lX %08lX\n", (unsigned long)a[4],
               (unsigned long)a[5], (unsigned long)a[6], (unsigned long)a[7],
               (unsigned long)a[8]);
    wifi_diag_log(); // For comparison with failed starts
}
#endif

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
#ifdef DSPICO_DSWIFI
    if ((!ok || res.status != 200) && (g_cfg.a7cmd53 || g_cfg.a7dma))
    {
        // Safety net for the ARM7 receive options: back to the proven path
        extern void Wifi_A7SetOpts(u32 opts);
        Wifi_A7SetOpts(a7_opts_base());
        g_cfg.a7cmd53 = g_cfg.a7dma = false;
        log_printf("ARM7 RX options turned OFF (first request failed), retrying\n");
        buf_free(&body);
        for (int i = 0; i < 60; i++)
            cothread_yield_irq(IRQ_VBLANK); // lets lwIP (DHCP) run
        ok = net_get("/api/heartbeat", false, &body, &res);
        net_log_result("heartbeat (retry)", &res);
    }
#endif
    if (!ok || res.status != 200)
    {
        buf_free(&body);
        step_mark(ST_SERVER, 3, NULL);
        fatal("RomM not reachable\nThe server did not answer. Details are in the log (verbose=1 adds a TLS trace).");
    }

    cJSON *root = cJSON_Parse(body.data);
    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "SYSTEM");
    const char *ver = json_str(sys, "VERSION");
    log_printf("RomM version: %s\n", ver ? ver : "?");
    step_mark(ST_SERVER, 2, ver ? ver : NULL);
    cJSON_Delete(root);
    buf_free(&body);

    // Second request on the same handle should reuse the TLS connection.
    log_printf("[2] GET /api/heartbeat (reuse)\n");
    ok = net_get("/api/heartbeat", false, &body, &res);
    net_log_result("heartbeat2", &res);
    buf_free(&body);
}

static char g_user[40];

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
        snprintf(g_user, sizeof(g_user), "%s", user ? user : "");
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
        fatal("Pairing could not start");
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

    char device_code_copy[160], code[48];
    snprintf(device_code_copy, sizeof(device_code_copy), "%s", device_code);
    snprintf(code, sizeof(code), "%s", user_code);

    char url[CFG_URL_MAX + 128], page[CFG_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s%s", g_cfg.url, vpath);
    snprintf(page, sizeof(page), "%s/pair/device", g_cfg.url);
    log_file_only("Pair URL: %s\n", url);
    cJSON_Delete(root);

    g_top_header = false;
    if (!top_show_qr(url))
        splash_top("URL too long for a QR code");

    char poll[256];
    snprintf(poll, sizeof(poll), "{\"device_code\":\"%s\"}", device_code_copy);

    uint32_t deadline = net_now_ms() + (uint32_t)expires_in * 1000;
    while ((int32_t)(deadline - net_now_ms()) > 0)
    {
        for (int s = 0; s < interval; s++)
        {
            ui_draw_pairing(gfx_bot, page, code, (int)((int32_t)(deadline - net_now_ms()) / 1000));
            if (!wait_ms_cancelable(1000))
            {
                splash_top(NULL);
                fatal("Pairing cancelled");
            }
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
            splash_top(NULL);
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
            splash_top(NULL);
            fatal("Pairing failed");
        }
        cJSON_Delete(err);
        buf_free(&body);
    }

    splash_top(NULL);
    fatal("Pairing code expired");
}

// ---------------------------------------------------------------------------
// Library: downloads
// ---------------------------------------------------------------------------

typedef struct
{
    uint32_t last_ui_ms;
    uint64_t size;           // Expected total
    const char *name;
    const char *phases[3];
} dl_ui_t;

static void progress_lines(uint64_t done, uint64_t total, uint32_t ms, char *l1, char *l2,
                           int len)
{
    char a[16], b[16];
    format_size(done, a, sizeof(a));
    format_size(total, b, sizeof(b));
    snprintf(l1, len, "%s / %s", a, b);
    uint32_t kbps = ms ? (uint32_t)((done * 1000 / 1024) / ms) : 0;
    if (kbps && total > done)
    {
        uint32_t left = (uint32_t)(((total - done) / 1024) / kbps);
        snprintf(l2, len, "%lu.%02lu MB/s  \xC2\xB7  %lu:%02lu left", (unsigned long)(kbps / 1024),
                 (unsigned long)((kbps % 1024) * 100 / 1024), (unsigned long)(left / 60),
                 (unsigned long)(left % 60));
    }
    else
        snprintf(l2, len, "%lu KB/s", (unsigned long)kbps);
}

static bool download_progress(void *ctx, uint64_t now, uint64_t total,
                              uint32_t elapsed_ms)
{
    dl_ui_t *ui = ctx;
    scanKeys();
    if (keysDown() & KEY_B)
        return false;
    if (elapsed_ms - ui->last_ui_ms < 250)
        return true;
    ui->last_ui_ms = elapsed_ms;

    uint64_t all = total ? total : ui->size;
    char l1[48], l2[64];
    progress_lines(now, all, elapsed_ms, l1, l2, sizeof(l1));
    ui_progress_t p = { "Downloading", ui->name, all ? (int)(now * 1000 / all) : -1, l1, l2,
                        { ui->phases[0], ui->phases[1], NULL }, 0, "B: cancel", UI_INFO };
    ui_draw_progress(gfx_bot, &p);
    gfx_present_now(); // No VBlank wait: it would cost download time
    return true;
}

typedef struct
{
    uint32_t t0, last_ui_ms;
    const char *name;
} unzip_ui_t;

static bool extract_progress(void *ctx, uint64_t done, uint64_t total)
{
    unzip_ui_t *ui = ctx;
    scanKeys();
    if (keysDown() & KEY_B)
        return false;
    uint32_t ms = net_now_ms() - ui->t0;
    if (ms - ui->last_ui_ms < 250)
        return true;
    ui->last_ui_ms = ms;
    char l1[48], l2[64];
    progress_lines(done, total, ms, l1, l2, sizeof(l1));
    ui_progress_t p = { "Unpacking", ui->name, total ? (int)(done * 1000 / total) : -1, l1, l2,
                        { "Download", "Unpack", NULL }, 1, "B: cancel", UI_INFO };
    ui_draw_progress(gfx_bot, &p);
    gfx_present_now();
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

// <base>/library.txt: "<rom id>\t<file name>" per completed download, so a
// game is found on the SD card whatever its file is called.
static void lib_record(int rom_id, const char *path)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char lp[CFG_PATH_MAX + 32];
    config_path(&g_cfg, "library.txt", lp, sizeof(lp));
    FILE *f = fopen(lp, "ab");
    if (!f)
        return;
    fprintf(f, "%d\t%s\n", rom_id, name);
    fclose(f);
}

// Downloads a ROM into <drive>/roms/<folder>/ (download.c). Archives are
// extracted afterwards. ask: confirm before replacing a ROM on the SD card.
static void download_to_library(int rom_id, const char *fs_name, uint64_t size,
                                const char *folder, bool ask)
{
    bool archive = download_is_archive(fs_name);
    dl_ui_t ui = { 0, size, fs_name, { "Download", archive ? "Unpack" : NULL, NULL } };

    uint64_t have = 0;
    char local[256];
    if (ask && download_local(&g_cfg, folder, fs_name, local, sizeof(local), &have))
    {
        char hs[16], body[320];
        format_size(have, hs, sizeof(hs));
        snprintf(body, sizeof(body), "%s (%s) is already in roms/%s/.", local, hs, folder);
        if (!message(UI_INFO, "Download again?", body, "A: download again  B: back"))
            return;
    }

    // An archive left by a failed extraction is reused
    uint64_t zsize = 0;
    bool have_zip = archive && download_exists(&g_cfg, folder, fs_name, &zsize) &&
                    zsize == size;

    // Download and extraction must fit: archive + ROM inside at the peak
    ui_progress_t chk = { "Downloading", fs_name, -1, "Checking free space...", NULL,
                          { ui.phases[0], ui.phases[1], NULL }, 0, NULL, UI_INFO };
    ui_draw_progress(gfx_bot, &chk);
    frame();
    space_check_t sc;
    bool fits = download_space_check(&g_cfg, rom_id, folder, fs_name, size, have_zip, &sc);
    char need_s[16], free_s[16], rom_s[16];
    format_size(sc.need, need_s, sizeof(need_s));
    format_size(sc.free, free_s, sizeof(free_s));
    format_size(sc.rom_size, rom_s, sizeof(rom_s));
    log_printf("\n[Space] need %s%s%s, free %s%s%s\n", need_s,
               sc.rom_size ? " incl. unpacked ROM " : "", sc.rom_size ? rom_s : "",
               free_s, sc.note[0] ? "; " : "", sc.note);
    if (!fits)
    {
        char body[128];
        snprintf(body, sizeof(body), "The download needs %s, the SD card has %s free.",
                 need_s, free_s);
        message(UI_ERROR, "Not enough space", body, NULL);
        return;
    }

    if (have_zip)
        log_printf("\n[Download] archive already on SD, unpacking it\n");
    else
    {
        download_result_t r;
#ifdef DSPICO_DSWIFI
        extern void Wifi_A7Info(u32 out[16]);
        u32 a7i0[16], a7i1[16];
        Wifi_A7Info(a7i0);
#endif
        bool ok = download_rom(&g_cfg, rom_id, fs_name, size, folder, download_progress,
                               &ui, &r);

        uint32_t kbps = r.ms ? (uint32_t)((r.bytes * 1000 / 1024) / r.ms) : 0;
        log_printf("\n[Download] %s -> %s\n", fs_name, r.path);
        log_printf("  %s: %llu B in %lu ms (%lu KB/s), %s, %d request(s)\n",
                   ok ? "done" : (r.cancelled ? "cancelled" : "FAILED"),
                   (unsigned long long)r.bytes, (unsigned long)r.ms, (unsigned long)kbps,
                   r.raw ? "DSpico DMA" : "DLDI", r.attempts);
        log_io(&r.io);
#ifdef DSPICO_DSWIFI
        Wifi_A7Info(a7i1);
        log_printf("  ARM7 RX: %lu CMD53 reg reads, %lu pkts DMA to ring, ring full %lu\n",
                   (unsigned long)(a7i1[10] - a7i0[10]), (unsigned long)(a7i1[11] - a7i0[11]),
                   (unsigned long)(a7i1[14] - a7i0[14]));
#endif
        if (!ok)
        {
            if (r.cancelled)
                message(UI_INFO, "Download cancelled",
                        "Start it again to continue where it stopped.", NULL);
            else
                message(UI_ERROR, "Download failed", r.error, NULL);
            return;
        }
        if (!archive)
        {
            lib_record(rom_id, r.path);
            char body[200];
            snprintf(body, sizeof(body), "%lu KB/s on average. Saved in roms/%s/.",
                     (unsigned long)kbps, folder);
            message(UI_OK, "Download complete", body, NULL);
            return;
        }
    }

    unzip_ui_t uz = { net_now_ms(), 0, fs_name };
    extract_result_t x;
    download_extract(&g_cfg, folder, fs_name, extract_progress, &uz, &x);
    log_extract(&x);
    if (x.ok)
    {
        lib_record(rom_id, x.path);
        char body[320];
        snprintf(body, sizeof(body), "%s is in roms/%s/.", x.path[0] ? strrchr(x.path, '/') + 1
                                                                      : x.entry, folder);
        message(UI_OK, "Ready to play", body, NULL);
    }
    else if (x.cancelled)
        message(UI_INFO, "Unpacking cancelled", "The archive stays on the SD card; "
                "downloading the game again unpacks it.", NULL);
    else
        message(UI_ERROR, "Unpacking failed", x.error, NULL);
}

// --- Library views --------------------------------------------------------

// Names of the files in <drive>/roms/<folder>/, sorted (for "on SD" marks).
typedef struct
{
    char **names;
    int count;
} dir_list_t;

// Case-insensitive: FAT names are, and names written on a PC may differ in case
static int cmp_str(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
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

// File name stem reduced to lower-case letters and digits (accents folded),
// e.g. "Pokémon - White (USA, Europe)" -> "pokemonwhiteusaeurope"
static void norm_stem(const char *name, char *out, int len)
{
    const char *dot = strrchr(name, '.');
    const unsigned char *p = (const unsigned char *)name;
    int n = 0;
    while (*p && (!dot || (const char *)p < dot) && n < len - 1)
    {
        unsigned c = *p++;
        if (c == 0xC3 && *p)
        {
            unsigned d = *p++ | 0x20;
            c = (d >= 0xA0 && d <= 0xA5) ? 'a' : d == 0xA7 ? 'c'
              : (d >= 0xA8 && d <= 0xAB) ? 'e' : (d >= 0xAC && d <= 0xAF) ? 'i'
              : d == 0xB1 ? 'n' : ((d >= 0xB2 && d <= 0xB6) || d == 0xB8) ? 'o'
              : (d >= 0xB9 && d <= 0xBC) ? 'u' : 0;
        }
        else if (c >= 'A' && c <= 'Z')
            c |= 0x20;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out[n++] = (char)c;
    }
    out[n] = 0;
}

static bool is_rom_file(const char *name)
{
    static const char *ext[] = { "nds", "dsi", "srl", "gba", "gbc", "gb" };
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (strcasecmp(dot + 1, ext[i]) == 0)
            return true;
    return false;
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

static int g_sync_total;
static const char *g_sync_name;

static void sync_progress(int fetched, int total)
{
    char l1[48];
    if (total)
        snprintf(l1, sizeof(l1), "%d / %d games", fetched, total);
    else
        snprintf(l1, sizeof(l1), "Asking RomM for changes...");
    ui_progress_t p = { "Updating game list", g_sync_name, total ? fetched * 1000 / total : -1,
                        l1, NULL, { NULL }, 0, NULL, UI_INFO };
    ui_draw_progress(gfx_bot, &p);
    gfx_present_now();
    g_sync_total = total;
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

// --- Save sync ---------------------------------------------------------------

static bool save_capable(const char *folder)
{
    static const char *f[] = { "nds", "dsi", "gba", "gb", "gbc" };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
        if (strcmp(folder, f[i]) == 0)
            return true;
    return false;
}

// ROMs left with a save conflict by the automatic sync (tile marks)
#define MAX_CONFLICTS 64
static int g_conflicts[MAX_CONFLICTS], g_n_conflicts;
static char g_sync_note[96];

static void conflict_set(int rom_id, bool on)
{
    for (int i = 0; i < g_n_conflicts; i++)
        if (g_conflicts[i] == rom_id)
        {
            if (!on)
                g_conflicts[i] = g_conflicts[--g_n_conflicts];
            return;
        }
    if (on && g_n_conflicts < MAX_CONFLICTS)
        g_conflicts[g_n_conflicts++] = rom_id;
}

static bool conflict_has(int rom_id)
{
    for (int i = 0; i < g_n_conflicts; i++)
        if (g_conflicts[i] == rom_id)
            return true;
    return false;
}

// At start: sync every ROM on the SD card whose save changed on one side only.
static bool g_startup = true;
static int g_ss_up, g_ss_down, g_ss_conflicts, g_ss_errors, g_ss_checked;

static void savesync_auto(const int *ids, const char **folders, int n)
{
    if (!g_cfg.token[0] || !ss_init(&g_cfg))
        return;
    int up = 0, down = 0, conflicts = 0, errors = 0, checked = 0;
    uint32_t t0 = net_now_ms();
    log_printf("\n[Save sync] slot \"%s\"\n", g_cfg.save_slot);
    for (int p = 0; p < n; p++)
    {
        if (!save_capable(folders[p]))
            continue;
        rom_index_t idx;
        idx_init(&idx, ids[p]);
        if (!idx_load(&g_cfg, &idx) || idx.count == 0)
        {
            idx_free(&idx);
            continue;
        }
        ss_list_t srv;
        if (!ss_fetch(&g_cfg, ids[p], 0, &srv))
        {
            errors++;
            idx_free(&idx);
            continue;
        }
        for (int i = 0; i < idx.count; i++)
        {
            ss_status_t st;
            ss_status(&g_cfg, &idx.e[i], folders[p], &srv, &st);
            if (st.action == SS_NONE)
                continue;
            checked++;
            if (st.action == SS_CONFLICT)
            {
                conflicts++;
                conflict_set(idx.e[i].id, true);
                log_printf("  %s: conflict (DSi %s, RomM %s)\n", idx.e[i].name,
                           st.local_hash, st.srv.hash[0] ? st.srv.hash : "-");
                continue;
            }
            if (st.action == SS_IN_SYNC && st.base_local[0])
                continue;
            ui_progress_t pr = { "Syncing saves", idx.e[i].name, -1,
                                 ss_action_name(st.action), NULL, { NULL }, 0, NULL, UI_INFO };
            ui_draw_progress(gfx_bot, &pr);
            frame();
            char err[128];
            log_printf("  %s: %s\n", idx.e[i].name, ss_action_name(st.action));
            if (ss_apply(&g_cfg, &st, st.action, err, sizeof(err)))
            {
                up += st.action == SS_UPLOAD;
                down += st.action == SS_DOWNLOAD;
            }
            else
            {
                errors++;
                log_printf("    failed: %s\n", err);
            }
        }
        ss_list_free(&srv);
        idx_free(&idx);
    }
    log_printf("  %d checked: %d uploaded, %d downloaded, %d conflicts, %d errors "
               "(%lu ms)\n", checked, up, down, conflicts, errors,
               (unsigned long)(net_now_ms() - t0));
    if (up || down || conflicts || errors)
        snprintf(g_sync_note, sizeof(g_sync_note),
                 "Saves at start: %d uploaded, %d downloaded%s%.0d%s%s%.0d%s", up, down,
                 conflicts ? ", " : "", conflicts, conflicts ? " conflict(s)" : "",
                 errors ? ", " : "", errors, errors ? " error(s)" : "");
    g_ss_up = up;
    g_ss_down = down;
    g_ss_conflicts = conflicts;
    g_ss_errors = errors;
    g_ss_checked = checked;
    if (g_startup)
    {
        char d[40];
        snprintf(d, sizeof(d), "%d up, %d down%s", up, down,
                 conflicts ? ", conflicts" : errors ? ", errors" : "");
        step_mark(ST_SAVES, errors ? 3 : 2, d);
    }
}

static void format_time(time_t t, char *out, int len)
{
    struct tm *tm = localtime(&t);
    snprintf(out, len, "%04d-%02d-%02d %02d:%02d", (tm->tm_year + 1900) % 10000,
             (tm->tm_mon + 1) % 100, tm->tm_mday % 100, tm->tm_hour % 100, tm->tm_min % 100);
}

// Save status for the top screen
static const char *save_text(int action, int *kind)
{
    switch (action)
    {
        case SS_IN_SYNC:  *kind = UI_OK;    return "in sync";
        case SS_UPLOAD:   *kind = UI_INFO;  return "newer on the DSi";
        case SS_DOWNLOAD: *kind = UI_INFO;  return "newer in RomM";
        case SS_CONFLICT: *kind = UI_WARN;  return "conflict (press Y)";
        case SS_ERROR:    *kind = UI_ERROR; return "error";
        default:          *kind = UI_INFO;  return "none yet";
    }
}

// Y: save sync screen for one game
static void save_sync_screen(const rom_entry_t *e, const char *folder)
{
    if (!save_capable(folder))
    {
        message(UI_INFO, "No save sync", "Saves are synced for DS, DSi, GBA, GB and GBC games.",
                NULL);
        return;
    }
    if (!g_cfg.token[0] || !ss_init(&g_cfg))
    {
        message(UI_ERROR, "Save sync unavailable", "The device could not be registered.", NULL);
        return;
    }
    ss_list_t srv;
    if (!ss_fetch(&g_cfg, 0, e->id, &srv))
    {
        message(UI_ERROR, "RomM not reachable", "The save list could not be loaded.", NULL);
        return;
    }
    ss_status_t st;
    ss_status(&g_cfg, e, folder, &srv, &st);
    ss_list_free(&srv);

    char ld[32] = "", ls[24] = "", sd[32] = "", ssz[24] = "";
    if (st.have_local)
    {
        // Pico Launcher writes saves with a fixed file date (2018-01-01), so
        // dates before 2020 carry no information
        struct tm *lt = localtime(&st.local_mtime);
        if (lt && lt->tm_year + 1900 >= 2020)
            format_time(st.local_mtime, ld, sizeof(ld));
        else
            snprintf(ld, sizeof(ld), "date unknown");
        format_size(st.local_size, ls, sizeof(ls));
    }
    if (st.have_server)
    {
        snprintf(sd, sizeof(sd), "%.10s %.5s UTC", st.srv.updated_at, st.srv.updated_at + 11);
        format_size(st.srv.size, ssz, sizeof(ssz));
    }

    int kind, action = st.action;
    const char *status = save_text(action, &kind);
    ui_savesync_t v = { e->name, st.have_local ? ld : NULL, ls, st.have_server ? sd : NULL, ssz,
                        status, kind, NULL, NULL };

    if (action == SS_NONE)
    {
        v.status = "No save on the DSi or in RomM";
        v.hint = "B: back";
        ui_draw_savesync(gfx_bot, &v);
        while (!(wait_key() & (KEY_A | KEY_B)))
            ;
        return;
    }
    if (action == SS_CONFLICT || action == SS_IN_SYNC || action == SS_ERROR)
    {
        v.status = action == SS_CONFLICT ? "Both changed since the last sync" : status;
        v.question = action == SS_CONFLICT ? "Keep which version?" : "Copy one side anyway?";
        v.hint = "L: keep DSi (upload)  R: keep RomM  B: back";
        ui_draw_savesync(gfx_bot, &v);
        while (1)
        {
            u16 k = wait_key();
            if ((k & KEY_L) && st.have_local)
            {
                action = SS_UPLOAD;
                break;
            }
            if ((k & KEY_R) && st.have_server)
            {
                action = SS_DOWNLOAD;
                break;
            }
            if (k & KEY_B)
                return;
        }
    }

    v.status = action == SS_UPLOAD ? "Uploading..." : "Downloading...";
    v.kind = UI_INFO;
    v.question = NULL;
    v.hint = NULL;
    ui_draw_savesync(gfx_bot, &v);
    frame();

    char err[128];
    log_printf("\n[Save sync] %s: %s\n", e->name, ss_action_name(action));
    if (ss_apply(&g_cfg, &st, action, err, sizeof(err)))
    {
        conflict_set(e->id, false);
        v.status = action == SS_UPLOAD ? "Uploaded to RomM" : "Downloaded to the DSi";
        v.kind = UI_OK;
        if (action == SS_DOWNLOAD)
            v.question = "The replaced DSi save is in romm/save-backup/.";
    }
    else
    {
        log_printf("  failed: %s\n", err);
        v.status = "Sync failed (see log)";
        v.kind = UI_ERROR;
    }
    v.hint = "A: OK";
    ui_draw_savesync(gfx_bot, &v);
    while (!(wait_key() & (KEY_A | KEY_B)))
        ;
}

// --- Tools (SELECT) --------------------------------------------------------------

static void show_log_tail(void)
{
    log_sync();
    char path[CFG_PATH_MAX + 32];
    config_path(&g_cfg, LOG_NAME, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    console_begin();
    if (!f)
        printf("No log file.\n");
    else
    {
        // Last ~1 KB (the console shows 24 lines of 32 characters)
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, sz > 1000 ? sz - 1000 : 0, SEEK_SET);
        static char buf[1001];
        size_t n = fread(buf, 1, 1000, f);
        buf[n] = 0;
        fclose(f);
        char *start = sz > 1000 ? strchr(buf, '\n') : buf;
        printf("%s", start ? start + (sz > 1000) : buf);
    }
    console_wait();
    console_end();
}

// e/folder: the selected game, NULL on the platform screen
// Returns 1 if "Download all artwork" was chosen (handled by the game screen)
static int tools_menu(const rom_entry_t *e, const char *folder, bool on_sd)
{
    const char *items[6];
    int act[6], n = 0, sel = 0;
    if (e)
    {
        items[n] = "Download all artwork (this platform)";
        act[n++] = 4;
    }
    int dark_item = n;
    items[n] = ui_dark ? "Dark mode: on" : "Dark mode: off";
    act[n++] = 5;
    items[n] = "Show log";
    act[n++] = 0;
    if (e)
    {
        items[n] = "Speed test (this game)";
        act[n++] = 1;
        if (on_sd && download_is_archive(e->fs_name))
        {
            items[n] = "Unzip CPU test (this game)";
            act[n++] = 2;
        }
    }
    items[n] = "Back";
    act[n++] = 3;
    while (1)
    {
        ui_draw_list(gfx_bot, "Tools", items, n, sel, "A: select  B: back");
        u16 k = wait_key();
        if ((k & KEY_DOWN) && sel < n - 1)
            sel++;
        else if ((k & KEY_UP) && sel > 0)
            sel--;
        else if (k & KEY_B)
            return 0;
        else if (k & KEY_A)
        {
            if (act[sel] == 0)
                show_log_tail();
            else if (act[sel] == 1)
            {
                console_begin();
                bench_run_suite(&g_cfg, e->id, e->fs_name, e->size);
                console_wait();
                console_end();
            }
            else if (act[sel] == 2)
            {
                char name[256], path[CFG_PATH_MAX + 256];
                console_begin();
                if (download_local(&g_cfg, folder, e->fs_name, name, sizeof(name), NULL))
                {
                    printf("Unzip CPU test\n%.31s\n\n", name);
                    download_target(&g_cfg, folder, name, path, sizeof(path));
                    unzip_bench(path);
                }
                console_wait();
                console_end();
            }
            else if (act[sel] == 4)
                return 1;
            else if (act[sel] == 5)
            {
                ui_set_dark(!ui_dark);
                cover_set_bg(ui_dark);
                ui_settings_save();
                items[dark_item] = ui_dark ? "Dark mode: on" : "Dark mode: off";
                return 2; // Caller redraws the top screen
            }
            else
                return 0;
        }
    }
}

// --- Strip navigation -------------------------------------------------------------

// Moves pos (index * 256) towards sel; true while it moves.
static bool strip_animate(int *pos, int sel)
{
    int target = sel * 256, d = target - *pos;
    if (d == 0)
        return false;
    int step = d / 2;
    if (step == 0)
        step = d > 0 ? 1 : -1;
    if (d > 0 && step < 48 && d > 48)
        step = 48;
    if (d < 0 && step > -48 && d < -48)
        step = -48;
    *pos += step;
    // Far jumps (letters): snap close to the target first
    if (*pos < target - 2 * 256)
        *pos = target - 2 * 256;
    if (*pos > target + 2 * 256)
        *pos = target + 2 * 256;
    return true;
}

static int strip_keys_move(u16 rep, int sel, int count)
{
    if ((rep & KEY_RIGHT) && sel < count - 1)
        return sel + 1;
    if ((rep & KEY_LEFT) && sel > 0)
        return sel - 1;
    return sel;
}

// --- Game screen -------------------------------------------------------------------

typedef struct
{
    rom_index_t idx;
    int platform_id;
    const char *platform_name;
    const char *folder;
    bool *on_sd;
    signed char *save;       // SS_* per game, -1 unknown
    ui_tile_t *tiles;
    char (*labels)[8];
    int8_t *letter;
    bool letters[27];
    ss_list_t srv;
    bool have_srv;
    uint8_t *cov;            // Strip image: COV_* per game (covers.h)
    uint8_t *covp;           // Top screen image: COV_* per game
    bool cov_scanned;        // Cover folder read (states valid)
    uint16_t **timg;         // Loaded tile images (TILE_S^2), NULL if not loaded
} games_t;

#define COV_FAILED 9         // Download failed this session (not retried)

static void games_tiles(games_t *g, int i)
{
    const rom_entry_t *e = &g->idx.e[i];
    uint8_t flags = g->on_sd[i] ? UI_TILE_ON_SD : 0;
    if (g->save[i] == SS_CONFLICT || (g->save[i] < 0 && conflict_has(e->id)))
        flags |= UI_TILE_WARN;
    g->tiles[i].flags = flags;
}

static bool games_build(games_t *g)
{
    int n = g->idx.count;
    g->on_sd = calloc(n, sizeof(bool));
    g->save = malloc(n);
    g->tiles = calloc(n, sizeof(ui_tile_t));
    g->labels = calloc(n, sizeof(*g->labels));
    g->letter = malloc(n);
    g->cov = calloc(n, 1);
    g->covp = calloc(n, 1);
    g->cov_scanned = false;
    g->timg = calloc(n, sizeof(uint16_t *));
    if (!g->on_sd || !g->save || !g->tiles || !g->labels || !g->letter || !g->cov || !g->covp ||
        !g->timg)
        return false;
    memset(g->save, -1, n);
    memset(g->letters, 0, sizeof(g->letters));

    dir_list_t onsd;
    dir_list_load(g->folder, &onsd);
    for (int i = 0; i < n; i++)
    {
        const rom_entry_t *e = &g->idx.e[i];
        char cand[256];
        for (int k = 0; !g->on_sd[i] &&
                        download_local_name(g->folder, e->fs_name, k, cand, sizeof(cand));
             k++)
            g->on_sd[i] = dir_list_has(&onsd, cand);
        ui_initials(e->name, g->labels[i], sizeof(g->labels[i]));
        g->tiles[i] = (ui_tile_t){ e->name, g->labels[i], ui_name_color(e->name), NULL, 0, 0, 0 };
        g->letter[i] = (int8_t)ui_letter_of(e->name);
        g->letters[g->letter[i]] = true;
    }

    // Downloads recorded in library.txt (file name may differ from RomM's)
    char lp[CFG_PATH_MAX + 32];
    config_path(&g_cfg, "library.txt", lp, sizeof(lp));
    FILE *f = fopen(lp, "rb");
    if (f)
    {
        char line[320];
        while (fgets(line, sizeof(line), f))
        {
            char *tab = strchr(line, '\t');
            if (!tab)
                continue;
            tab[strcspn(tab, "\r\n")] = 0;
            int id = atoi(line);
            for (int i = 0; i < n; i++)
                if (g->idx.e[i].id == id && !g->on_sd[i])
                    g->on_sd[i] = dir_list_has(&onsd, tab + 1);
        }
        fclose(f);
    }
    // Last resort: same name apart from punctuation, case and accents (ROMs
    // unpacked or copied with a slightly different name)
    char (*stems)[96] = onsd.count ? malloc(onsd.count * sizeof(*stems)) : NULL;
    if (stems)
    {
        for (int j = 0; j < onsd.count; j++)
        {
            stems[j][0] = 0;
            if (is_rom_file(onsd.names[j]))
                norm_stem(onsd.names[j], stems[j], sizeof(stems[j]));
        }
        for (int i = 0; i < n; i++)
        {
            if (g->on_sd[i])
                continue;
            char st[96];
            norm_stem(g->idx.e[i].fs_name, st, sizeof(st));
            for (int j = 0; st[0] && j < onsd.count; j++)
                if (strcmp(st, stems[j]) == 0)
                {
                    g->on_sd[i] = true;
                    log_printf("[Library] %s: matched %s\n", g->idx.e[i].fs_name, onsd.names[j]);
                    break;
                }
        }
        free(stems);
    }
    // Diagnostics: ROM files in the folder that match no game
    int unmatched = 0;
    for (int j = 0; j < onsd.count && unmatched < 8; j++)
    {
        if (!is_rom_file(onsd.names[j]))
            continue;
        bool used = false;
        for (int i = 0; i < n && !used; i++)
        {
            char cand[256];
            for (int k = 0; !used && download_local_name(g->folder, g->idx.e[i].fs_name, k,
                                                         cand, sizeof(cand));
                 k++)
                used = strcasecmp(cand, onsd.names[j]) == 0;
        }
        if (!used)
        {
            log_printf("[Library] roms/%s/%s: no exact match in RomM\n", g->folder,
                       onsd.names[j]);
            unmatched++;
        }
    }
    for (int i = 0; i < n; i++)
        games_tiles(g, i);
    dir_list_free(&onsd);
    return true;
}

static void games_free(games_t *g)
{
    if (g->timg)
        for (int i = 0; i < g->idx.count; i++)
            free(g->timg[i]);
    free(g->timg);
    free(g->cov);
    free(g->covp);
    g->covp = NULL;
    g->timg = NULL;
    g->cov = NULL;
    free(g->on_sd);
    free(g->save);
    free(g->tiles);
    free(g->labels);
    free(g->letter);
    g->on_sd = NULL;
    g->save = NULL;
    g->tiles = NULL;
    g->labels = NULL;
    g->letter = NULL;
}

// L/R: first game of the previous/next letter
static int letter_jump(const games_t *g, int sel, int dir)
{
    int n = g->idx.count, cur = g->letter[sel];
    if (dir < 0)
    {
        // To the first game of this letter, else of the previous letter
        int first = -1;
        for (int i = 0; i < n; i++)
            if (g->letter[i] == cur)
            {
                first = i;
                break;
            }
        if (first >= 0 && first < sel)
            return first;
        for (int l = cur - 1; l >= 0; l--)
            for (int i = 0; i < n; i++)
                if (g->letter[i] == l)
                    return i;
        return sel;
    }
    for (int l = cur + 1; l < 27; l++)
        for (int i = 0; i < n; i++)
            if (g->letter[i] == l)
                return i;
    return sel;
}

static int g_top_cover_w, g_top_cover_h;
static uint16_t g_top_cover[COVER_W * COVER_H];

static void game_top(games_t *g, int sel, const char *summary, const uint16_t *cover)
{
    const rom_entry_t *e = &g->idx.e[sel];
    ui_game_t t = { e->name, g->platform_name, e->regions, e->genres, e->players, summary,
                    e->year, e->size, g->on_sd[sel], download_is_archive(e->fs_name),
                    NULL, UI_INFO, cover, g_top_cover_w, g_top_cover_h, clock_str() };
    if (save_capable(g->folder) && g->on_sd[sel])
    {
        if (g->save[sel] >= 0)
            t.save = save_text(g->save[sel], &t.save_kind);
        else
            t.save = "checking...";
    }
    ui_wifi_level = wifi_level();
    ui_draw_game_top(gfx_top, &t);
    g_top_header = true;
}

// Summary from the metadata file (first paragraph after the key lines)
static void game_summary(int rom_id, char *out, int len, char *publisher, int plen)
{
    static char meta[2048];
    out[0] = 0;
    if (publisher)
        publisher[0] = 0;
    if (!idx_meta(&g_cfg, rom_id, meta, sizeof(meta)))
        return;
    char *pub = strstr(meta, "publisher=");
    if (pub && publisher)
    {
        char *nl = strchr(pub, '\n');
        snprintf(publisher, plen, "%.*s", nl ? (int)(nl - pub - 10) : 60, pub + 10);
    }
    char *summary = strstr(meta, "\n\n");
    if (summary)
        snprintf(out, len, "%s", summary + 2);
}

// A: details page (description, download, save sync)
static void game_page(games_t *g, int sel)
{
    const rom_entry_t *e = &g->idx.e[sel];
    static char summary[1200];
    char publisher[64], sub[160], sz[16];
    game_summary(e->id, summary, sizeof(summary), publisher, sizeof(publisher));
    format_size(e->size, sz, sizeof(sz));
    snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s%s%s", e->fs_name, sz,
             publisher[0] ? "  \xC2\xB7  " : "", publisher);
    int scroll = 0;
    bool cover_tried = false;
    while (1)
    {
        ui_draw_page(gfx_bot, e->name, sub, summary[0] ? summary : "No description in RomM.",
                     g->on_sd[sel]
                         ? "\xE2\x96\xB2\xE2\x96\xBC scroll  A: download again  Y: save  B: back"
                         : "\xE2\x96\xB2\xE2\x96\xBC scroll  A: download  B: back",
                     &scroll);
        if (!cover_tried)
        {
            // Opened before the background worker got to this cover: fetch it
            // now, after the page is on screen
            cover_tried = true;
            frame();
            if (g->cov_scanned && g->covp[sel] == COV_MISSING)
            {
                int r = cover_fetch(&g_cfg, g->platform_id, e, COV_TOP);
                g->covp[sel] = (uint8_t)(r == COV_MISSING ? COV_FAILED : r);
                if (r == COV_CACHED && cover_load(&g_cfg, g->platform_id, e->id, COV_TOP,
                                                  g_top_cover, &g_top_cover_w, &g_top_cover_h))
                    game_top(g, sel, NULL, g_top_cover);
            }
        }
        u16 k = wait_key(), rep = keysDownRepeat();
        if (rep & KEY_DOWN)
            scroll++;
        else if (rep & KEY_UP)
            scroll--;
        else if (k & KEY_R)
            scroll += 8;
        else if (k & KEY_L)
            scroll -= 8;
        else if (k & KEY_B)
            return;
        else if (k & KEY_A)
        {
            download_to_library(e->id, e->fs_name, e->size, g->folder, false);
            g->on_sd[sel] = download_local(&g_cfg, g->folder, e->fs_name, NULL, 0, NULL);
            games_tiles(g, sel);
            return;
        }
        else if ((k & KEY_Y) && g->on_sd[sel])
        {
            save_sync_screen(e, g->folder);
            g->save[sel] = -1;
            return;
        }
    }
}

// Save status of one game from the platform's server list (no network)
static void games_save_status(games_t *g, int sel)
{
    if (g->save[sel] >= 0 || !g->on_sd[sel] || !save_capable(g->folder) || !g->have_srv)
        return;
    ss_status_t st;
    ss_status(&g_cfg, &g->idx.e[sel], g->folder, &g->srv, &st);
    g->save[sel] = (signed char)st.action;
    games_tiles(g, sel);
}

// Background update of the game list and the platform's save list, so the
// strip shows the index from the SD card right away. Network calls of the
// main thread wait for it first (one curl handle).
typedef struct
{
    int platform_id;
    bool want_srv;
    rom_index_t idx;
    idx_sync_result_t r;
    bool ok;
    ss_list_t srv;
    bool have_srv;
    volatile bool done;
} bgsync_t;

static int bgsync_thread(void *arg)
{
    bgsync_t *b = arg;
    b->ok = idx_sync(&g_cfg, &b->idx, NULL, &b->r);
    if (b->want_srv)
        b->have_srv = ss_fetch(&g_cfg, b->platform_id, 0, &b->srv);
    b->done = true;
    return 0;
}

static bool bgsync_start(bgsync_t *b, int platform_id, bool want_srv)
{
    memset(b, 0, sizeof(*b));
    b->platform_id = platform_id;
    b->want_srv = want_srv;
    idx_init(&b->idx, platform_id);
    idx_load(&g_cfg, &b->idx);
    if (cothread_create(bgsync_thread, b, 32 * 1024, COTHREAD_DETACHED) < 0)
    {
        idx_free(&b->idx);
        b->done = true;
        return false;
    }
    return true;
}

// Waits until the background update has finished
static void bgsync_wait(bgsync_t *b, bool *running)
{
    for (int f = 0; *running && !b->done; f++)
    {
        if (f == 20)
        {
            ui_progress_t p = { "Updating game list", NULL, -1, "One moment...", NULL,
                                { NULL }, 0, NULL, UI_INFO };
            ui_draw_progress(gfx_bot, &p);
        }
        frame();
    }
}

static int find_rom(const rom_index_t *idx, int id)
{
    for (int i = 0; i < idx->count; i++)
        if (idx->e[i].id == id)
            return i;
    return -1;
}

// --- Covers: background worker and image cache --------------------------------
//
// The worker checks the SD cache nearest-first around the selection and
// downloads missing covers. Decoding blocks the CPU for ~0.1 s, so a download
// only starts while the selection rests. Main-thread network calls pause the
// worker first (one curl handle).

typedef struct
{
    games_t *g;
    volatile int focus;
    volatile bool resting;
    volatile bool pause, stop;
    volatile bool busy, exited;
    volatile int fetched;    // Covers added (main reloads images when it changes)
} covw_t;

static int covw_thread(void *arg)
{
    covw_t *w = arg;
    while (!w->stop)
    {
        if (w->pause)
        {
            // Sleep, not spin: a paused worker must not take CPU from downloads
            w->busy = false;
            cothread_yield_irq(IRQ_VBLANK);
            continue;
        }
        w->busy = true;
        games_t *g = w->g;
        int n = g->idx.count, f = w->focus, pick = -1, kind = COV_TILE;
        if (!g->cov_scanned)
        {
            // One read of the platform's cover folder for all states
            cover_scan(&g_cfg, g->platform_id, g->idx.e, n, g->cov, g->covp);
            g->cov_scanned = true;
            w->fetched++;
            pick = -2;
        }
        else if (w->resting)
        {
            // Downloads only while the selection rests: decoding blocks the
            // CPU. First the top screen image of the selected game, then strip
            // images nearest to it.
            if (f < n && g->covp[f] == COV_MISSING)
            {
                pick = f;
                kind = COV_TOP;
            }
            for (int d = 0; d < n && pick < 0; d++)
                for (int side = -1; side <= 1 && pick < 0; side += 2)
                {
                    int i = f + side * d;
                    if (i >= 0 && i < n && !(d == 0 && side == 1) && g->cov[i] == COV_MISSING)
                        pick = i;
                }
        }
        if (pick >= 0)
        {
            // Main pauses the worker only between jobs, so g is still valid
            int r = cover_fetch(&g_cfg, g->platform_id, &g->idx.e[pick], kind);
            uint8_t v = (uint8_t)(r == COV_MISSING ? COV_FAILED : r);
            if (kind == COV_TOP)
                g->covp[pick] = v;
            else
                g->cov[pick] = v;
            if (r == COV_CACHED)
                w->fetched++;
        }
        w->busy = false;
        if (pick == -1)
        {
            // Nothing to do near the focus: wait a moment
            for (int i = 0; i < 10 && !w->stop && !w->pause; i++)
                cothread_yield_irq(IRQ_VBLANK);
        }
        else
            cothread_yield();
    }
    w->busy = false;
    w->exited = true;
    return 0;
}

static bool covw_start(covw_t *w, games_t *g)
{
    memset((void *)w, 0, sizeof(*w));
    w->g = g;
    if (cothread_create(covw_thread, (void *)w, 32 * 1024, COTHREAD_DETACHED) < 0)
    {
        w->exited = true;
        return false;
    }
    return true;
}

// Waits until the worker is between jobs and keeps it there
static void covw_pause(covw_t *w)
{
    w->pause = true;
    while (w->busy && !w->exited)
        frame();
}

static void covw_stop(covw_t *w)
{
    w->stop = true;
    while (!w->exited)
        frame();
}

// Loads cached tiles near the selection (one SD read per frame) and frees far ones
static void games_load_tiles(games_t *g, int sel)
{
    int n = g->idx.count;
    for (int i = 0; i < n; i++)
        if (g->timg[i] && (i < sel - 12 || i > sel + 12))
        {
            free(g->timg[i]);
            g->timg[i] = NULL;
            g->tiles[i].img = NULL;
        }
    for (int d = 0; d <= 4; d++)
        for (int side = -1; side <= 1; side += 2)
        {
            int i = sel + side * d;
            if (i < 0 || i >= n || g->cov[i] != COV_CACHED || g->timg[i])
                continue;
            uint16_t *img = malloc(TILE_S * TILE_S * 2);
            int iw, ih;
            if (img && cover_load(&g_cfg, g->platform_id, g->idx.e[i].id, COV_TILE, img, &iw, &ih))
            {
                g->timg[i] = img;
                g->tiles[i].img = img;
                g->tiles[i].img_w = iw;
                g->tiles[i].img_h = ih;
            }
            else
            {
                free(img);
                g->cov[i] = COV_NONE;
            }
            return;
        }
}

// Tools: downloads every missing image of the platform in one go (like a
// scraper), so browsing only reads the SD card afterwards. B cancels.
static void art_bulk(games_t *g)
{
    int n = g->idx.count;
    if (!g->cov_scanned)
    {
        cover_scan(&g_cfg, g->platform_id, g->idx.e, n, g->cov, g->covp);
        g->cov_scanned = true;
    }
    int total = 0;
    for (int i = 0; i < n; i++)
        total += (g->cov[i] == COV_MISSING || g->cov[i] == COV_FAILED) +
                 (g->covp[i] == COV_MISSING || g->covp[i] == COV_FAILED);
    if (total == 0)
    {
        message(UI_OK, "All artwork is on the SD card", NULL, NULL);
        return;
    }
    log_printf("\n[Artwork] %s: %d images to load\n", g->platform_name, total);
    int done = 0, loaded = 0, none = 0, failed = 0;
    uint32_t t0 = net_now_ms();
    bool cancel = false;
    for (int i = 0; i < n && !cancel; i++)
        for (int kind = COV_TILE; kind <= COV_TOP && !cancel; kind++)
        {
            uint8_t *st = kind == COV_TILE ? &g->cov[i] : &g->covp[i];
            if (*st != COV_MISSING && *st != COV_FAILED)
                continue;
            char l1[48], l2[64];
            snprintf(l1, sizeof(l1), "%d / %d images", done, total);
            uint32_t ms = net_now_ms() - t0;
            if (done)
            {
                uint32_t left = ms / done * (total - done) / 1000;
                snprintf(l2, sizeof(l2), "%d loaded, %d without art  \xC2\xB7  %lu:%02lu left",
                         loaded, none, (unsigned long)(left / 60), (unsigned long)(left % 60));
            }
            else
                snprintf(l2, sizeof(l2), "Starting...");
            ui_progress_t p = { "Downloading artwork", g->idx.e[i].name, done * 1000 / total,
                                l1, l2, { NULL }, 0, "B: cancel", UI_INFO };
            ui_draw_progress(gfx_bot, &p);
            gfx_present_now();
            scanKeys();
            if (keysDown() & KEY_B)
            {
                cancel = true;
                break;
            }
            int r = cover_fetch(&g_cfg, g->platform_id, &g->idx.e[i], kind);
            *st = (uint8_t)(r == COV_MISSING ? COV_FAILED : r);
            loaded += r == COV_CACHED;
            none += r == COV_NONE;
            failed += r == COV_MISSING;
            done++;
        }
    uint32_t ms = net_now_ms() - t0;
    log_printf("  %d loaded, %d without artwork, %d failed%s, %lu ms (%lu ms per image)\n",
               loaded, none, failed, cancel ? ", cancelled" : "", (unsigned long)ms,
               (unsigned long)(done ? ms / done : 0));
    char body[160];
    snprintf(body, sizeof(body), "%d images loaded, %d games without artwork%s%.0d%s.", loaded,
             none, failed ? ", " : "", failed, failed ? " failed (try again later)" : "");
    message(cancel ? UI_INFO : failed ? UI_WARN : UI_OK,
            cancel ? "Artwork download cancelled" : "Artwork downloaded", body, NULL);
}

static void browse_platform(int platform_id, const char *platform_name,
                            const char *folder)
{
    games_t g = { 0 };
    g.platform_id = platform_id;
    g.platform_name = platform_name;
    g.folder = folder;
    idx_init(&g.idx, platform_id);
    bool have = idx_load(&g_cfg, &g.idx) && g.idx.count > 0;
    bool want_srv = save_capable(folder) && g_cfg.token[0] && ss_init(&g_cfg);

    bool full = false;
    int sel = 0;
    static bgsync_t bg;
    bool bg_running = false;
    static covw_t cw;
    bool cw_running = false;
    uint16_t *top_cover = g_top_cover;
    while (!g_quit)
    {
        if (full || !have)
        {
            // In the foreground: first sync of the platform, or X (full)
            if (full)
            {
                idx_free(&g.idx);
                idx_init(&g.idx, platform_id);
            }
            g_sync_name = platform_name;
            sync_progress(0, 0);
            idx_sync_result_t r;
            bool ok = idx_sync(&g_cfg, &g.idx, sync_progress, &r);
            log_printf("\n[Index %s] %s: %d fetched, %d removed, %d ROMs, %d req, %lu ms\n",
                       platform_name, ok ? (r.full ? "full" : "update") : "FAILED",
                       r.fetched, r.removed, g.idx.count, r.requests, (unsigned long)r.ms);
            if (!ok)
            {
                log_printf("  %s\n", r.error);
                if (g.idx.count == 0)
                {
                    message(UI_ERROR, "Game list not available", r.error, NULL);
                    break;
                }
                message(UI_WARN, "Game list not updated",
                        "RomM could not be reached. Showing the list saved on the SD card.",
                        NULL);
            }
            if (want_srv)
                g.have_srv = ss_fetch(&g_cfg, platform_id, 0, &g.srv);
            have = true;
        }
        else
            bg_running = bgsync_start(&bg, platform_id, want_srv);
        full = false;

        if (g.idx.count == 0)
        {
            message(UI_INFO, "No games", "RomM has no games for this platform.", NULL);
            break;
        }
        if (!games_build(&g))
            break;

        if (sel >= g.idx.count)
            sel = g.idx.count - 1;
        int pos = sel * 256, idle = 0, shown = -1, cover_for = -1, seen_fetched = 0;
        bool leave = false, offline = false;
        u16 pending = 0; // Key held back until the background update is taken
        static char summary[600];
        char sub[40];
        while (!leave && !g_quit)
        {
            if (bg_running && bg.done)
            {
                // Background update finished: take the new list if it changed
                bg_running = false;
                log_printf("\n[Index %s] %s (background): %d fetched, %d removed, %d ROMs, "
                           "%d req, %lu ms\n", platform_name,
                           bg.ok ? (bg.r.full ? "full" : "update") : "FAILED", bg.r.fetched,
                           bg.r.removed, bg.idx.count, bg.r.requests, (unsigned long)bg.r.ms);
                offline = !bg.ok;
                if (!bg.ok)
                    log_printf("  %s\n", bg.r.error);
                if (bg.ok && (bg.r.fetched || bg.r.removed) && bg.idx.count > 0)
                {
                    int id = g.idx.e[sel].id;
                    if (cw_running)
                        covw_pause(&cw);
                    games_free(&g);
                    idx_free(&g.idx);
                    g.idx = bg.idx;
                    if (!games_build(&g))
                    {
                        leave = true;
                        break;
                    }
                    int i = find_rom(&g.idx, id);
                    sel = i >= 0 ? i : (sel < g.idx.count ? sel : g.idx.count - 1);
                    pos = sel * 256;
                    cover_for = -1;
                    cw.pause = false;
                }
                else
                    idx_free(&bg.idx);
                if (bg.have_srv)
                {
                    if (g.have_srv)
                        ss_list_free(&g.srv);
                    g.srv = bg.srv;
                    g.have_srv = true;
                    memset(g.save, -1, g.idx.count);
                }
                shown = -1;
            }
            // Covers start once the list is final (the network is free then)
            if (!bg_running && !cw_running && g_cfg.token[0])
                cw_running = covw_start(&cw, &g);

            const rom_entry_t *e = &g.idx.e[sel];
            const uint16_t *cov_img = cover_for == sel ? top_cover : NULL;
            // Details once the selection rests (SD reads, save hashing)
            if (shown != sel)
            {
                summary[0] = 0;
                game_top(&g, sel, NULL, NULL);
                shown = sel;
                idle = 0;
            }
            else if (++idle == 3 && g.covp[sel] == COV_CACHED)
            {
                // Cover after 50 ms of rest (an SD read would slow fast scrolling)
                if (cover_load(&g_cfg, platform_id, e->id, COV_TOP, top_cover, &g_top_cover_w,
                               &g_top_cover_h))
                    cover_for = sel;
                game_top(&g, sel, NULL, cover_for == sel ? top_cover : NULL);
            }
            else if (idle == 12)
            {
                game_summary(e->id, summary, sizeof(summary), NULL, 0);
                games_save_status(&g, sel);
                game_top(&g, sel, summary, cover_for == sel ? top_cover : NULL);
            }
            else if (idle > 12 && g.save[sel] < 0 && g.have_srv && g.on_sd[sel] &&
                     save_capable(folder))
            {
                // The save list arrived after the selection came to rest
                games_save_status(&g, sel);
                game_top(&g, sel, summary, cov_img);
            }
            else if (idle > 12 && cover_for != sel && g.covp[sel] == COV_CACHED)
            {
                // The cover arrived while the selection rests
                if (cover_load(&g_cfg, platform_id, e->id, COV_TOP, top_cover, &g_top_cover_w,
                               &g_top_cover_h))
                    cover_for = sel;
                else
                    g.covp[sel] = COV_NONE; // Try once
                game_top(&g, sel, summary, cover_for == sel ? top_cover : NULL);
            }
            if (cw_running)
            {
                cw.focus = sel;
                cw.resting = idle > 30;
                (void)seen_fetched;
                // Strip images: one SD read per frame, only when the strip
                // stands still (no reads while scrolling)
                if (pos == sel * 256 && !(keysHeld() & (KEY_LEFT | KEY_RIGHT)))
                    games_load_tiles(&g, sel);
            }

            snprintf(sub, sizeof(sub), "%d game%s", g.idx.count, g.idx.count == 1 ? "" : "s");
            ui_strip_t s = { platform_name, sub, g.tiles, g.idx.count, sel, pos, NULL, g.letters,
                             g.letter[sel],
                             "\xE2\x97\x80\xE2\x96\xB6 game  L/R letter  A details  Y save  B back" };
            s.status = bg_running ? 1 : offline ? 2 : 0;
            s.anim = (int)g_frames;
            ui_draw_strip(gfx_bot, &s);
            frame();
            strip_animate(&pos, sel);

            u16 k = keysDown(), rep = keysDownRepeat();
            if (pending)
            {
                k = pending;
                rep = 0;
                pending = 0;
            }
            int nsel = strip_keys_move(rep, sel, g.idx.count);
            if (k & KEY_R)
                nsel = letter_jump(&g, sel, 1);
            else if (k & KEY_L)
                nsel = letter_jump(&g, sel, -1);
            if (nsel != sel)
            {
                sel = nsel;
                continue;
            }
            // Everything below may use the network: the update finishes first,
            // is taken at the top of the loop, then the key is handled
            const u16 net_keys = KEY_START | KEY_B | KEY_X | KEY_SELECT | KEY_Y | KEY_A;
            if ((k & net_keys) && bg_running)
            {
                bgsync_wait(&bg, &bg_running);
                pending = k & net_keys;
                continue;
            }
            if ((k & net_keys) && cw_running)
                covw_pause(&cw);
            if (k & KEY_START)
                g_quit = confirm_quit();
            else if (k & KEY_B)
                leave = true;
            else if (k & KEY_X)
            {
                full = true;
                break;
            }
            else if (k & KEY_SELECT)
            {
                if (tools_menu(e, folder, g.on_sd[sel]) == 1)
                    art_bulk(&g);
                cover_for = -1;
                shown = -1;
            }
            else if (k & KEY_Y)
            {
                if (!g.on_sd[sel] && save_capable(folder))
                    message(UI_INFO, "Download the game first",
                            "Its save is synced once the game is on the SD card.", NULL);
                else
                    save_sync_screen(e, folder);
                g.save[sel] = -1;
                shown = -1;
            }
            else if (k & KEY_A)
            {
                game_page(&g, sel);
                g.save[sel] = -1;
                shown = -1;
            }
            if (cw_running && !leave && !g_quit)
                cw.pause = false;
        }
        if (cw_running)
        {
            covw_stop(&cw);
            cw_running = false;
        }
        if (bg_running)
        {
            bgsync_wait(&bg, &bg_running);
            bg_running = false;
            idx_free(&bg.idx);
            if (bg.have_srv)
                ss_list_free(&bg.srv);
        }
        if (g.have_srv)
            ss_list_free(&g.srv);
        g.have_srv = false;
        games_free(&g);
        if (leave)
            break;
    }
    games_free(&g);
    idx_free(&g.idx);
}

// --- Platform screen ---------------------------------------------------------------

typedef struct
{
    int id, roms, on_sd;
    char name[28];
    char folder[32];
    char label[8];
} plat_t;

static void platform_label(const char *folder, char *out, int len)
{
    if (strcmp(folder, "dsi") == 0)
    {
        snprintf(out, len, "DSi");
        return;
    }
    int n = 0;
    for (; folder[n] && n < 4 && n < len - 1; n++)
        out[n] = (char)(folder[n] >= 'a' && folder[n] <= 'z' ? folder[n] - 32 : folder[n]);
    out[n] = 0;
}

static int count_files(const char *folder)
{
    dir_list_t d;
    dir_list_load(folder, &d);
    int n = d.count;
    dir_list_free(&d);
    return n;
}

static void step_platforms(void)
{
    buf_t body = { 0 };
    net_result_t res;

    step_mark(ST_LIBRARY, 1, NULL);
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
        message(UI_INFO, "No platforms", "RomM has no platforms with games.", "A: exit");
        return;
    }

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
        platform_label(pl[i].folder, pl[i].label, sizeof(pl[i].label));
        pl[i].on_sd = -1;
    }
    cJSON_Delete(root);
    char d[24];
    snprintf(d, sizeof(d), "%d platforms", count);
    step_mark(ST_LIBRARY, 2, d);

    int hidden[64], n_hidden = 0;
    hidden_load(hidden, 64, &n_hidden);
    bool edit = false;

    // Automatic save sync for the visible platforms (conflicts are left for
    // the game screen, where the user decides)
    {
        int ids[64], n_ids = 0;
        const char *folders[64];
        for (int i = 0; i < count && n_ids < 64; i++)
        {
            bool hid = false;
            for (int j = 0; j < n_hidden; j++)
                hid |= hidden[j] == pl[i].id;
            if (!hid)
            {
                ids[n_ids] = pl[i].id;
                folders[n_ids++] = pl[i].folder;
            }
        }
        step_mark(ST_SAVES, 1, NULL);
        savesync_auto(ids, folders, n_ids);
        g_startup = false; // From here on no start-up step screen
    }

    ui_tile_t *tiles = calloc(count, sizeof(ui_tile_t));
    int *map = calloc(count, sizeof(int));
    int sel = 0, pos = 0, shown = -1;
    while (!g_quit)
    {
        int n = 0;
        for (int i = 0; i < count; i++)
        {
            bool hid = false;
            for (int j = 0; j < n_hidden; j++)
                hid |= hidden[j] == pl[i].id;
            if (hid && !edit)
                continue;
            tiles[n] = (ui_tile_t){ pl[i].name, pl[i].label, ui_name_color(pl[i].name), NULL,
                                    0, 0, (uint8_t)(hid ? UI_TILE_DIMMED : 0) };
            map[n++] = i;
        }
        if (sel >= n)
            sel = n ? n - 1 : 0;
        if (pos > sel * 256 + 3 * 256 || pos < sel * 256 - 3 * 256)
            pos = sel * 256;

        plat_t *p = n ? &pl[map[sel]] : NULL;
        bool hid_sel = n && (tiles[sel].flags & UI_TILE_DIMMED);
        if (p && shown != map[sel])
        {
            if (p->on_sd < 0)
                p->on_sd = count_files(p->folder);
            ui_platform_t t = { p->name, p->label, p->folder, p->roms, p->on_sd, hid_sel,
                                g_sync_note[0] ? g_sync_note : NULL, clock_str() };
            ui_wifi_level = wifi_level();
            ui_draw_platform_top(gfx_top, &t);
            g_top_header = true;
            shown = map[sel];
        }

        char sub[24], cap[64];
        snprintf(sub, sizeof(sub), "%d %s", n, edit ? "in total" : "shown");
        if (p)
            snprintf(cap, sizeof(cap), "%d games  \xC2\xB7  %d on SD", p->roms,
                     p->on_sd < 0 ? 0 : p->on_sd);
        ui_strip_t s = { edit ? "Show / hide platforms" : "Platforms", sub, tiles, n, sel, pos,
                         p ? cap : NULL, NULL, 0,
                         edit ? "A: hide / show  Y: done"
                              : "A open  X sync all saves  Y show/hide  SELECT tools" };
        ui_draw_strip(gfx_bot, &s);
        frame();
        strip_animate(&pos, sel);

        u16 k = keysDown(), rep = keysDownRepeat();
        int nsel = strip_keys_move(rep, sel, n);
        if (k & KEY_R)
            nsel = n ? (sel + 4 < n ? sel + 4 : n - 1) : 0;
        else if (k & KEY_L)
            nsel = sel >= 4 ? sel - 4 : 0;
        if (nsel != sel)
        {
            sel = nsel;
            continue;
        }
        if (k & KEY_START)
            g_quit = confirm_quit();
        else if (k & KEY_Y)
        {
            edit = !edit;
            shown = -1;
            // Keep the same platform selected
            if (p)
            {
                int want = map[sel], j = 0;
                for (int i = 0; i < count; i++)
                {
                    bool hid = false;
                    for (int h = 0; h < n_hidden; h++)
                        hid |= hidden[h] == pl[i].id;
                    if (hid && !edit)
                        continue;
                    if (i == want)
                    {
                        sel = j;
                        pos = sel * 256;
                        break;
                    }
                    j++;
                }
            }
        }
        else if (k & KEY_SELECT)
        {
            (void)tools_menu(NULL, NULL, false);
            shown = -1;
        }
        else if ((k & KEY_X) && !edit)
        {
            // All saves of the visible platforms: safe directions sync,
            // conflicts stay for the game screen (Y)
            int ids[64], n_ids = 0;
            const char *folders[64];
            for (int i = 0; i < count && n_ids < 64; i++)
            {
                bool hid = false;
                for (int j = 0; j < n_hidden; j++)
                    hid |= hidden[j] == pl[i].id;
                if (!hid)
                {
                    ids[n_ids] = pl[i].id;
                    folders[n_ids++] = pl[i].folder;
                }
            }
            ui_progress_t pr = { "Syncing saves", "Checking all platforms...", -1, NULL, NULL,
                                 { NULL }, 0, NULL, UI_INFO };
            ui_draw_progress(gfx_bot, &pr);
            frame();
            savesync_auto(ids, folders, n_ids);
            char body[240];
            int n = snprintf(body, sizeof(body), "%d uploaded, %d downloaded.", g_ss_up,
                             g_ss_down);
            if (g_ss_conflicts && n < (int)sizeof(body))
                n += snprintf(body + n, sizeof(body) - n,
                              "\n\n%d conflict(s): these games show an orange \"!\". "
                              "Open one and press Y to choose a version.", g_ss_conflicts);
            if (g_ss_errors && n < (int)sizeof(body))
                snprintf(body + n, sizeof(body) - n, "\n\n%d error(s), see the log.",
                         g_ss_errors);
            message(g_ss_errors ? UI_ERROR : g_ss_conflicts ? UI_WARN : UI_OK,
                    g_ss_up + g_ss_down ? "Saves synced" : "All saves up to date", body, NULL);
            shown = -1;
        }
        else if ((k & KEY_A) && p && edit)
        {
            int id = p->id, j;
            for (j = 0; j < n_hidden && hidden[j] != id; j++)
                ;
            if (j < n_hidden)
                hidden[j] = hidden[--n_hidden];
            else if (n_hidden < 64)
                hidden[n_hidden++] = id;
            hidden_save(hidden, n_hidden);
            shown = -1;
        }
        else if ((k & KEY_A) && p)
        {
            browse_platform(p->id, p->name, p->folder);
            p->on_sd = -1;
            shown = -1;
        }
    }

    free(tiles);
    free(map);
    free(pl);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    defaultExceptionHandler();

    gfx_init();
    log_set_echo(false);
    keysSetRepeat(14, 3);
    splash_top(NULL);
    step_mark(ST_SD, 1, NULL);

#ifdef DLDI_FORCE_ARM9
    // Run the SD driver on the ARM9 so the ARM7 is free for Wi-Fi.
    dldiSetMode(DLDI_MODE_ARM9);
#endif
    if (!fatInitDefault())
    {
        step_mark(ST_SD, 3, NULL);
        fatal("No SD card access\nThe SD card could not be read (FAT init failed).");
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
        fatal("Crypto self-test failed\nChaCha20-Poly1305 gives wrong results.");
    log_printf("Crypto self-test: OK\n");

    if (!have_cfg)
        fatal("No configuration\nromm/config.ini is missing or has no url= line (see README).");
    step_mark(ST_SD, 2, NULL);

    log_printf("Config: %sconfig.ini\n", g_cfg.base_dir);
    log_printf("Save slot: %s\n", g_cfg.save_slot);
    cover_tile_box2d = g_cfg.art_box2d;
    ui_settings_load();
    log_printf("Artwork in the strip: %s\n", g_cfg.art_box2d ? "box2d" : "cover");
    log_file_only("URL: %s insecure=%d verbose=%d\n", g_cfg.url, g_cfg.insecure,
                  g_cfg.verbose);
    {
        // Host name only, for the top screen
        const char *h = strstr(g_cfg.url, "://");
        h = h ? h + 3 : g_cfg.url;
        char host[64];
        snprintf(host, sizeof(host), "%.*s", (int)strcspn(h, "/:"), h);
        splash_top(host);
    }

    step_mark(ST_WIFI, 1, NULL);
    step_wifi();
    prof_init();

    if (!net_init(&g_cfg))
        fatal("Network init failed");

    if (g_cfg.insecure)
        log_printf("WARNING: cert check disabled\n");

    step_mark(ST_SERVER, 1, NULL);
    step_heartbeat();

    step_mark(ST_LOGIN, 1, NULL);
    if (!token_valid())
    {
        config_delete_token(&g_cfg);
        g_cfg.token[0] = '\0';
        step_pair();
        if (!token_valid())
            fatal("Sign-in failed\nThe new token does not work.");
    }
    step_mark(ST_LOGIN, 2, g_user);

    step_platforms();

    log_printf("\nDone. Log: %s" LOG_NAME "\n", g_cfg.base_dir);
    ui_draw_message(gfx_bot, UI_INFO, "Closing", "Disconnecting Wi-Fi...", "");
    frame();
    net_cleanup();
    wifi_shutdown();
    log_close();
    return 0;
}

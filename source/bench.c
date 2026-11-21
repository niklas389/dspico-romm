// SPDX-License-Identifier: MIT
//
// Speed-test suite. Separates the possible bottlenecks of a download:
//   - round-trip time (bounds a single TCP connection to window / RTT)
//   - CPU cost of TLS record decryption (ChaCha20-Poly1305 / AES-128-GCM)
//   - SD card write speed
//   - network throughput with 1, 2 and 4 parallel connections
//   - optionally plain HTTP (http_url= in config.ini) to see the TLS cost

#include "bench.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>
#include <nds.h>

#include "log.h"
#include "net.h"
#include "prof.h"
#include "dl.h"
#include "sdraw.h"
#include "sdwriter.h"

#define NET_TEST_BYTES (8u * 1024 * 1024)
#define CRYPTO_BYTES   (256u * 1024)
#define CRYPTO_ROUNDS  8
#define SD_TEST_BYTES  (4u * 1024 * 1024)

static uint32_t kbps(uint64_t bytes, uint32_t ms)
{
    return ms ? (uint32_t)((bytes * 1000 / 1024) / ms) : 0;
}

static bool cancel_pressed(void)
{
    scanKeys();
    return keysDown() & KEY_B;
}

// ---------------------------------------------------------------------------
// RTT via TCP connect time
// ---------------------------------------------------------------------------

static void parse_host_port(const char *url, char *host, size_t hlen, char *port,
                            size_t plen)
{
    const char *p = strstr(url, "://");
    bool https = strncmp(url, "https", 5) == 0;
    p = p ? p + 3 : url;

    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < hlen)
        host[i++] = *p++;
    host[i] = '\0';

    if (*p == ':')
    {
        p++;
        i = 0;
        while (*p && *p != '/' && i + 1 < plen)
            port[i++] = *p++;
        port[i] = '\0';
    }
    else
    {
        snprintf(port, plen, "%s", https ? "443" : "80");
    }
}

static void bench_rtt(const config_t *cfg, uint32_t *best, uint32_t *median)
{
    char host[128], port[8];
    parse_host_port(cfg->url, host, sizeof(host), port, sizeof(port));

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *ai = NULL;

    *best = *median = 0;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai)
    {
        log_printf("RTT: DNS failed\n");
        return;
    }

    uint32_t samples[7];
    int n = 0;
    for (int i = 0; i < 7; i++)
    {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0)
            continue;
        uint32_t t0 = net_now_ms();
        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        uint32_t dt = net_now_ms() - t0;
        close(s);
        if (rc == 0)
            samples[n++] = dt;
    }
    freeaddrinfo(ai);

    if (n == 0)
    {
        log_printf("RTT: connect failed\n");
        return;
    }

    // Insertion sort, n <= 7
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && samples[j] < samples[j - 1]; j--)
        {
            uint32_t t = samples[j];
            samples[j] = samples[j - 1];
            samples[j - 1] = t;
        }

    *best = samples[0];
    *median = samples[n / 2];
    log_printf("RTT (TCP connect x%d): min %lu, med %lu ms\n", n,
               (unsigned long)*best, (unsigned long)*median);
}

// ---------------------------------------------------------------------------
// Crypto
// ---------------------------------------------------------------------------

static void bench_crypto(uint32_t *chacha_kbps, uint32_t *gcm_kbps)
{
    *chacha_kbps = *gcm_kbps = 0;

    unsigned char *in = malloc(CRYPTO_BYTES);
    unsigned char *out = malloc(CRYPTO_BYTES);
    if (!in || !out)
    {
        log_printf("crypto: out of memory\n");
        free(in);
        free(out);
        return;
    }
    memset(in, 0xA5, CRYPTO_BYTES);

    unsigned char key[32] = { 1 }, nonce[12] = { 2 }, tag[16];

    mbedtls_chachapoly_context cp;
    mbedtls_chachapoly_init(&cp);
    mbedtls_chachapoly_setkey(&cp, key);
    uint32_t t0 = net_now_ms();
    for (int i = 0; i < CRYPTO_ROUNDS; i++)
        mbedtls_chachapoly_encrypt_and_tag(&cp, CRYPTO_BYTES, nonce, NULL, 0, in,
                                           out, tag);
    uint32_t dt = net_now_ms() - t0;
    mbedtls_chachapoly_free(&cp);
    *chacha_kbps = kbps((uint64_t)CRYPTO_BYTES * CRYPTO_ROUNDS, dt);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    t0 = net_now_ms();
    for (int i = 0; i < CRYPTO_ROUNDS; i++)
        mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, CRYPTO_BYTES, nonce,
                                  sizeof(nonce), NULL, 0, in, out, sizeof(tag), tag);
    dt = net_now_ms() - t0;
    mbedtls_gcm_free(&gcm);
    *gcm_kbps = kbps((uint64_t)CRYPTO_BYTES * CRYPTO_ROUNDS, dt);

    free(in);
    free(out);

    log_printf("CPU ChaCha20-Poly1305: %lu KB/s\n", (unsigned long)*chacha_kbps);
    log_printf("CPU AES-128-GCM:       %lu KB/s\n", (unsigned long)*gcm_kbps);
}

// ---------------------------------------------------------------------------
// SD write
// ---------------------------------------------------------------------------

// Reserves len bytes for f by seeking past the end and writing one byte.
// FatFs then allocates the whole cluster chain at once instead of extending
// the file (and updating the FAT) on every write. Returns to offset 0.
static bool sd_preallocate(FILE *f, uint64_t len)
{
    if (fseek(f, (long)len - 1, SEEK_SET) != 0 || fputc(0, f) == EOF ||
        fflush(f) != 0 || fseek(f, 0, SEEK_SET) != 0)
        return false;
    return true;
}

static uint32_t bench_sd_one(const char *path, size_t block, bool prealloc)
{
    unsigned char *buf = malloc(block);
    FILE *f = fopen(path, "wb");
    if (!buf || !f)
    {
        free(buf);
        if (f)
            fclose(f);
        return 0;
    }
    memset(buf, 0x5A, block);
    setvbuf(f, NULL, _IONBF, 0); // Our blocks go straight to FatFs

    uint32_t t0 = net_now_ms();
    bool ok = !prealloc || sd_preallocate(f, SD_TEST_BYTES);
    size_t written = 0;
    while (ok && written < SD_TEST_BYTES)
    {
        if (fwrite(buf, 1, block, f) != block)
            break;
        written += block;
    }
    fclose(f);
    uint32_t dt = net_now_ms() - t0;

    free(buf);
    remove(path);
    return kbps(written, dt);
}

// Returns the best SD write speed; logs every combination.
// Direct DSpico sector writes (sdraw.c) vs the DLDI driver, on a test file:
// write a pattern, read it back through FatFs and compare.
static void bench_sdraw(const config_t *cfg)
{
    log_printf("SD raw (DLDI: %s)\n", sdraw_dldi_name());
    if (!sdraw_is_dspico())
    {
        log_printf("  skipped: not the DSpico driver\n");
        return;
    }

    const uint32_t size = 2 * 1024 * 1024, chunk = 64 * 1024;
    char dir[CFG_PATH_MAX + 16], file[CFG_PATH_MAX + 48];
    config_path(cfg, "test", dir, sizeof(dir));
    mkdir(dir, 0777);
    snprintf(file, sizeof(file), "%s/sdraw.bin", dir);

    uint32_t *buf = aligned_alloc(32, chunk);
    uint32_t *rd = aligned_alloc(32, chunk);
    FILE *f = fopen(file, "w+b");
    if (!buf || !rd || !f)
    {
        log_printf("  setup failed\n");
        goto out;
    }
    setvbuf(f, NULL, _IONBF, 0);

    static sdraw_extent_t ext[64];
    int n = sdraw_map(f, size, ext, 64);
    if (n <= 0)
    {
        log_printf("  map failed (%d)\n", n);
        goto out;
    }
    log_printf("  %d extent(s), first LBA %lu (%lu sectors)\n", n,
               (unsigned long)ext[0].lba, (unsigned long)ext[0].count);

    for (int mode = 0; mode < 3; mode++)
    {
        const char *name = mode == 0 ? "DLDI" : mode == 1 ? "raw CPU" : "raw DMA";
        uint32_t seed = 0x9E3779B9u * (mode + 1) + net_now_ms();

        // Write
        uint64_t t0 = prof_ticks();
        bool ok = true;
        int e = 0;
        uint32_t e_off = 0; // sectors used in ext[e]
        if (mode == 0)
            fseek(f, 0, SEEK_SET);
        for (uint32_t off = 0; off < size && ok; off += chunk)
        {
            for (uint32_t i = 0; i < chunk / 4; i++)
                buf[i] = seed ^ ((off / 4 + i) * 2654435761u);
            if (mode == 0)
            {
                ok = fwrite(buf, 1, chunk, f) == chunk;
                continue;
            }
            uint32_t sec = chunk / 512, done = 0;
            while (done < sec && ok)
            {
                uint32_t k = ext[e].count - e_off;
                if (k > sec - done)
                    k = sec - done;
                ok = sdraw_write(ext[e].lba + e_off, buf + done * 128, k, mode == 2);
                done += k;
                e_off += k;
                if (e_off == ext[e].count)
                {
                    e++;
                    e_off = 0;
                }
            }
        }
        if (mode == 0)
            fflush(f);
        uint32_t ms = prof_ticks_to_ms(prof_ticks() - t0);

        // Read back through FatFs and compare
        uint32_t bad = 0;
        fseek(f, 0, SEEK_SET);
        for (uint32_t off = 0; off < size && ok; off += chunk)
        {
            if (fread(rd, 1, chunk, f) != chunk)
            {
                bad = UINT32_MAX;
                break;
            }
            for (uint32_t i = 0; i < chunk / 4; i++)
                if (rd[i] != (seed ^ ((off / 4 + i) * 2654435761u)))
                    bad++;
        }
        log_printf("  %-7s %s %4lu KB/s, verify %s\n", name, ok ? "ok" : "FAIL",
                   (unsigned long)(ms ? (size / 1024) * 1000 / ms : 0),
                   bad == 0 ? "ok" : "MISMATCH");
        if (bad)
            log_file_only("  %s: %lu bad words\n", name, (unsigned long)bad);
        if (!ok)
            break;
    }

out:
    if (f)
        fclose(f);
    remove(file);
    free(buf);
    free(rd);
}

static uint32_t bench_sd(const config_t *cfg)
{
    char dir[CFG_PATH_MAX + 16], path[CFG_PATH_MAX + 48];
    config_path(cfg, "test", dir, sizeof(dir));
    mkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/sdbench.bin", dir);

    const size_t blocks[3] = { 64 * 1024, 256 * 1024, 1024 * 1024 };
    uint32_t best = 0;
    log_printf("SD write KB/s  plain  prealloc\n");
    for (int i = 0; i < 3; i++)
    {
        uint32_t plain = bench_sd_one(path, blocks[i], false);
        uint32_t pre = bench_sd_one(path, blocks[i], true);
        log_printf("  %4uK block   %5lu  %5lu\n", (unsigned)(blocks[i] / 1024),
                   (unsigned long)plain, (unsigned long)pre);
        if (plain > best)
            best = plain;
        if (pre > best)
            best = pre;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

#ifdef DSPICO_DSWIFI
extern u32 Wifi_Arm7BusyLines(void);
extern int dswifi_rx_debug(char *buf, int len);
extern bool dswifi_rx_zero_copy;

static void rx_debug_log(const char *why)
{
    static char buf[1024];
    dswifi_rx_debug(buf, sizeof(buf));
    log_file_only("  RX ring (%s), Wi-Fi %s, ARM7 lines %lu:\n%s", why,
                  net_wifi_status(), (unsigned long)Wifi_Arm7BusyLines(), buf);
}
#else
static void rx_debug_log(const char *why) { (void)why; }
#endif

// Stall detection: logs the zero-copy RX state once if no data arrived for 2 s.
static uint64_t g_last_bytes;
static uint32_t g_last_change_ms;
static bool g_stall_logged;

static void stall_reset(void)
{
    net_wifi_ensure();
    g_last_bytes = 0;
    g_last_change_ms = 0;
    g_stall_logged = false;
}

static bool net_progress(void *ctx, uint64_t now, uint64_t total, uint32_t ms)
{
    (void)ctx;
    if (now != g_last_bytes)
    {
        g_last_bytes = now;
        g_last_change_ms = ms;
    }
    else if (!g_stall_logged && ms - g_last_change_ms > 2000)
    {
        g_stall_logged = true;
        log_printf("  stall at %llu B\n", (unsigned long long)now);
        rx_debug_log("stall");
    }
    printf("\x1b[22;0H%3lu%%  %4lu KB/s   ",
           total ? (unsigned long)(now * 100 / total) : 0,
           (unsigned long)kbps(now, ms));
    return !cancel_pressed();
}

typedef struct
{
    const char *label;
    uint32_t raw_kbps;     // bytes / wall time
    uint32_t steady_kbps;  // bytes / (wall time - setup)
    uint32_t setup_ms;
    uint32_t rx_queued;    // ARM7 -> ARM9 packets during the test
    uint32_t rx_lost;      // ... dropped because the ring buffer was full
    uint32_t wait_pct;     // Main thread blocked waiting for the network
    uint32_t sd_pct;       // Time in fwrite() (SD card)
    uint32_t a7_pct;       // ARM7 time in the Wi-Fi handlers
    bool ok;
} net_row_t;

static const config_t *g_bench_cfg;

extern volatile int g_memcpy_stats_on;
void memcpy_stats_reset(void);
int memcpy_stats_get(uint32_t *out, int max_entries);

static void memcpy_stats_log(const char *label)
{
    static uint32_t t[64 * 4];
    int n = memcpy_stats_get(t, 64);
    if (n == 0)
        return; // Built without MEMCPY_STATS
    log_file_only("memcpy call sites (%s): caller calls bytes src_region\n", label);
    for (int i = 0; i < n; i++)
        log_file_only("  mcs %08lx %lu %lu %02lx\n", (unsigned long)t[4 * i],
                      (unsigned long)t[4 * i + 1], (unsigned long)t[4 * i + 2],
                      (unsigned long)t[4 * i + 3]);
}

static void save_samples(const config_t *cfg, const char *name)
{
    uint32_t n = prof_sampling_stop();
    char path[CFG_PATH_MAX + 32];
    config_path(cfg, name, path, sizeof(path));
    log_printf("  %lu samples -> %s%s\n", (unsigned long)n, name,
               prof_samples_save(path) ? "" : " (save failed)");
}

static void prof_fill(net_row_t *row, uint32_t total_ms)
{
    if (!total_ms)
        return;
    uint32_t wait = prof_ticks_to_ms(prof_wait_ticks());
    uint32_t sd = prof_ticks_to_ms(prof_sd_ticks());
    row->wait_pct = wait * 100 / total_ms;
    row->sd_pct = sd * 100 / total_ms;
    log_file_only("  wait %lu ms, sd %lu ms, total %lu ms\n", (unsigned long)wait,
                  (unsigned long)sd, (unsigned long)total_ms);
}

#ifndef DSPICO_DSWIFI
static u32 Wifi_Arm7BusyLines(void) { return 0; }
#endif
static u32 g_a7_lines0, g_a7_t0;
static u32 g_a7p0[8];

#ifdef DSPICO_DSWIFI
extern void Wifi_Arm7Prof(u32 out[8]);
#else
static void Wifi_Arm7Prof(u32 out[8]) { memset(out, 0, 8 * sizeof(u32)); }
#endif

static void a7_start(void)
{
    g_a7_lines0 = Wifi_Arm7BusyLines();
    g_a7_t0 = net_now_ms();
    Wifi_Arm7Prof(g_a7p0);
}

// ARM7 breakdown since a7_start(): microseconds from scanline sums.
static void a7_log(void)
{
    u32 p[8];
    Wifi_Arm7Prof(p);
    for (int i = 0; i < 8; i++)
        p[i] -= g_a7p0[i];
    u32 ms = net_now_ms() - g_a7_t0;
    u32 busy_us = (u32)((u64)(Wifi_Arm7BusyLines() - g_a7_lines0) * 6356 / 100);
#define US(l) ((u32)((u64)(l) * 6356 / 100))
    u32 irqs = p[5], pkts = p[6], c52 = p[0], blks = p[2];
    log_file_only("  ARM7 %lu ms busy of %lu: irqs %lu, pkts %lu, cmd52 %lu (%lu ms), "
                  "blk %lu x %lu B (%lu ms), queue copy %lu ms\n",
                  (unsigned long)(busy_us / 1000), (unsigned long)ms,
                  (unsigned long)irqs, (unsigned long)pkts, (unsigned long)c52,
                  (unsigned long)(US(p[1]) / 1000), (unsigned long)blks,
                  (unsigned long)(blks ? p[7] / blks : 0),
                  (unsigned long)(US(p[3]) / 1000), (unsigned long)(US(p[4]) / 1000));
    if (pkts)
        log_file_only("  ARM7 per packet: %lu us busy, %lu cmd52 (%lu us), blk %lu us, "
                      "queue %lu us, %lu pkts/irq\n",
                      (unsigned long)(busy_us / pkts), (unsigned long)(c52 / pkts),
                      (unsigned long)(US(p[1]) / pkts), (unsigned long)(US(p[3]) / pkts),
                      (unsigned long)(US(p[4]) / pkts),
                      (unsigned long)(irqs ? pkts / irqs : 0));
#undef US
}

// ARM7 Wi-Fi handler load since a7_start(), in percent (1 line = 63.56 us).
static u32 a7_pct(void)
{
    u32 ms = net_now_ms() - g_a7_t0;
    u64 us = (u64)(Wifi_Arm7BusyLines() - g_a7_lines0) * 6356 / 100;
    return ms ? (u32)(us / 10 / ms) : 0;
}

static void stats_delta(net_row_t *row, const net_wifi_stats_t *a)
{
    row->a7_pct = a7_pct();
    a7_log();
    log_file_only("  ARM7 Wi-Fi busy %lu%%\n", (unsigned long)row->a7_pct);
    net_wifi_stats_t b;
    net_wifi_stats(&b);
    row->rx_queued = b.rx_queued - a->rx_queued;
    row->rx_lost = b.rx_lost - a->rx_lost;
    if (b.tx_rejected != a->tx_rejected)
        log_printf("  TX rejected: %lu\n",
                   (unsigned long)(b.tx_rejected - a->tx_rejected));
}

static bool run_net(const char *label, const char *base, const char *path,
                    uint64_t len, int conns, bool sample, net_row_t *row)
{
    log_printf("%s...\n", label);
    net_wifi_stats_t st;
    net_wifi_stats(&st);
    a7_start();
    net_par_result_t r;
    prof_reset();
    if (sample)
    {
        memcpy_stats_reset();
        g_memcpy_stats_on = 1;
        prof_sampling_start();
    }
    stall_reset();
    net_download_parallel(base, path, len, conns, net_progress, NULL, &r);
    rx_debug_log(label);
    if (sample)
    {
        g_memcpy_stats_on = 0;
        save_samples(g_bench_cfg, "prof-" TCP_WND_LABEL "-net.bin");
        memcpy_stats_log("net");
    }
    prof_fill(row, r.total_ms);
    stats_delta(row, &st);
    printf("\x1b[22;0H                    \n");

    row->label = label;
    row->ok = r.ok;
    row->setup_ms = r.setup_ms;
    row->raw_kbps = kbps(r.bytes, r.total_ms);
    uint32_t steady_ms = r.total_ms > r.setup_ms ? r.total_ms - r.setup_ms : 0;
    row->steady_kbps = kbps(r.bytes, steady_ms);

    log_file_only("  bytes %llu, total %lu ms, setup %lu ms, HTTP %ld, %s\n",
                  (unsigned long long)r.bytes, (unsigned long)r.total_ms,
                  (unsigned long)r.setup_ms, r.status,
                  r.tls_cipher ? r.tls_cipher : "plain/none");
    if (!r.ok)
        log_printf("  FAILED: %s\n", r.error);
    return strcmp(r.error, "cancelled") != 0;
}

// Single keep-alive connection, body written to the SD card like a real
// download. Shows how much SD writes slow down reception.
__attribute__((unused))
static bool run_net_sd(const config_t *cfg, const char *path, uint64_t len,
                       size_t buf_size, const char *label, net_row_t *row)
{
    log_printf("%s...\n", label);

    char dir[CFG_PATH_MAX + 16], file[CFG_PATH_MAX + 48];
    config_path(cfg, "test", dir, sizeof(dir));
    mkdir(dir, 0777);
    snprintf(file, sizeof(file), "%s/dlbench.bin", dir);

    memset(row, 0, sizeof(*row));
    row->label = label;

    FILE *f = fopen(file, "wb");
    if (!f)
    {
        log_printf("  can't create %s\n", file);
        return true;
    }
    // Reserve the file. buf_size 0: unbuffered, curl's chunks go straight to
    // FatFs (no copy); otherwise through a stdio buffer of that size.
    if (buf_size)
        setvbuf(f, NULL, _IOFBF, buf_size);
    else
        setvbuf(f, NULL, _IONBF, 0);
    if (!sd_preallocate(f, len))
        log_printf("  preallocation failed\n");

    net_wifi_stats_t st;
    net_wifi_stats(&st);
    a7_start();
    net_result_t r;
    prof_reset();
    (void)buf_size;
    bool sample = false; // Profiled in the async test, the real download path
    if (sample)
    {
        memcpy_stats_reset();
        g_memcpy_stats_on = 1;
        prof_sampling_start();
    }
    stall_reset();
    bool ok = net_download_range(path, 0, len - 1, f, net_progress, NULL, &r);
    rx_debug_log(label);
    if (sample)
    {
        g_memcpy_stats_on = 0;
        save_samples(cfg, "prof-" TCP_WND_LABEL "-sd.bin");
        memcpy_stats_log("sd");
    }
    prof_fill(row, r.total_ms);
    fclose(f);
    stats_delta(row, &st);
    remove(file);
    printf("\x1b[22;0H                    \n");

    row->ok = ok && r.body_bytes == len && r.status == 206;
    row->raw_kbps = kbps(r.body_bytes, r.total_ms);
    row->steady_kbps = row->raw_kbps; // Connection is reused: no setup
    log_file_only("  bytes %llu, total %lu ms, HTTP %ld\n",
                  (unsigned long long)r.body_bytes, (unsigned long)r.total_ms,
                  r.status);
    if (!row->ok)
        log_printf("  FAILED: %s\n", r.error[0] ? r.error : "short/bad status");
    return r.curl_code != 42; // CURLE_ABORTED_BY_CALLBACK: user pressed B
}

// Like run_net_sd, but SD writes run in a background cothread while the main
// thread waits for the network.
static bool run_net_sd_async(const config_t *cfg, const char *path, uint64_t len,
                             const char *label, bool sample, bool use_dl,
                             net_row_t *row)
{
    log_printf("%s...\n", label);

    char dir[CFG_PATH_MAX + 16], file[CFG_PATH_MAX + 48];
    config_path(cfg, "test", dir, sizeof(dir));
    mkdir(dir, 0777);
    snprintf(file, sizeof(file), "%s/dlbench.bin", dir);

    memset(row, 0, sizeof(*row));
    row->label = label;

    FILE *f = fopen(file, "wb");
    if (!f)
    {
        log_printf("  can't create %s\n", file);
        return true;
    }
    setvbuf(f, NULL, _IONBF, 0); // The writer already writes in chunks
    if (!sd_preallocate(f, len))
        log_printf("  preallocation failed\n");

    sdwriter_t *w = sdw_open(f, 1024 * 1024, 32 * 1024);
    if (!w)
    {
        log_printf("  writer init failed\n");
        fclose(f);
        remove(file);
        return true;
    }

    net_wifi_stats_t st;
    net_wifi_stats(&st);
    a7_start();
    net_result_t r;
    prof_reset();
    memcpy_stats_reset();
    g_memcpy_stats_on = 1;
    if (sample)
        prof_sampling_start();
    stall_reset();
    bool ok = use_dl
            ? dl_get_range(path, 0, len - 1, w, net_progress, NULL, &r)
            : net_download_range_async(path, 0, len - 1, w, net_progress, NULL, &r);
    rx_debug_log(label);
    // Count the time to drain the ring: the download isn't done before that.
    uint64_t t0 = prof_ticks();
    uint64_t stall_ticks = sdw_stall_ticks(w);
    bool wok = sdw_close(w); // Drains the ring, then frees the writer
    uint32_t drain_ms = prof_ticks_to_ms(prof_ticks() - t0);
    g_memcpy_stats_on = 0;
    if (sample)
    {
        save_samples(cfg, "prof-" TCP_WND_LABEL "-sd.bin");
        memcpy_stats_log("sd");
    }
    fclose(f);
    stats_delta(row, &st);
    remove(file);
    printf("\x1b[22;0H                    \n");

    uint32_t total_ms = r.total_ms + drain_ms;
    prof_fill(row, total_ms);
    row->sd_pct = 0; // SD time overlaps the wait; reported separately below
    row->ok = ok && wok && r.body_bytes == len && r.status == 206;
    row->raw_kbps = kbps(r.body_bytes, total_ms);
    row->steady_kbps = row->raw_kbps;
    if (use_dl)
        log_file_only("  %s%s\n", r.zero_copy_tls ? "zero-copy TLS" : "Mbed TLS decrypt",
                      r.error[0] ? "" : "");
    log_printf("  ring full %lu ms, drain %lu ms\n",
               (unsigned long)prof_ticks_to_ms(stall_ticks),
               (unsigned long)drain_ms);
    if (!row->ok)
        log_printf("  FAILED: %s\n", r.error[0] ? r.error : "write/short/bad status");
    return r.curl_code != 42;
}

// The curl-free client (dl.c), body discarded. A new connection, so TLS
// setup is included like in run_net().
__attribute__((unused))
static bool run_dl_net(const char *label, const char *path, uint64_t len,
                       bool sample, net_row_t *row)
{
    log_printf("%s...\n", label);
    memset(row, 0, sizeof(*row));
    row->label = label;
    net_wifi_stats_t st;
    net_wifi_stats(&st);
    a7_start();
    net_result_t r;
    prof_reset();
    if (sample)
        prof_sampling_start();
    stall_reset();
    dl_close();
    bool ok = dl_get_range(path, 0, len - 1, NULL, net_progress, NULL, &r);
    rx_debug_log(label);
    if (sample)
        save_samples(g_bench_cfg, "prof-" TCP_WND_LABEL "-net.bin");
    prof_fill(row, r.total_ms);
    stats_delta(row, &st);
    printf("\x1b[22;0H                    \n");

    row->ok = ok && r.body_bytes == len && r.status == 206;
    row->setup_ms = r.ttfb_ms;
    row->raw_kbps = kbps(r.body_bytes, r.total_ms);
    uint32_t steady_ms = r.total_ms > r.ttfb_ms ? r.total_ms - r.ttfb_ms : 0;
    row->steady_kbps = kbps(r.body_bytes, steady_ms);
    log_file_only("  bytes %llu, total %lu ms, connect %lu, tls %lu, ttfb %lu ms, "
                  "HTTP %ld, %s %s%s\n",
                  (unsigned long long)r.body_bytes, (unsigned long)r.total_ms,
                  (unsigned long)r.connect_ms, (unsigned long)r.tls_ms,
                  (unsigned long)r.ttfb_ms, r.status,
                  r.tls_version ? r.tls_version : "-", r.tls_cipher ? r.tls_cipher : "-",
                  r.zero_copy_tls ? ", zero-copy TLS" : "");
    if (!row->ok)
        log_printf("  FAILED: %s\n", r.error[0] ? r.error : "short/bad status");
    return r.curl_code != 42;
}

void bench_run_suite(const config_t *cfg, int rom_id, const char *fs_name,
                     uint64_t rom_size)
{
    consoleClear();
    log_printf("=== Speed test (TCP_WND %s) ===\n", TCP_WND_LABEL);
    log_printf("ROM: %.30s\nB: cancel\n", fs_name);

    uint64_t len = rom_size < NET_TEST_BYTES ? rom_size : NET_TEST_BYTES;
    if (len < 1024 * 1024)
    {
        log_printf("Pick a ROM of at least 1 MB.\n");
        return;
    }

    char *escaped = net_escape(fs_name);
    if (!escaped)
        return;
    char path[512];
    snprintf(path, sizeof(path), "/api/roms/%d/content/%s", rom_id, escaped);
    free(escaped);

    uint32_t rtt_min, rtt_med, chacha, gcm, sd;
    bench_rtt(cfg, &rtt_min, &rtt_med);
    bench_crypto(&chacha, &gcm);
    sd = bench_sd(cfg);
    bench_sdraw(cfg);

    g_bench_cfg = cfg;
    net_row_t rows[9];
    int nrows = 0;
    // libcurl (v0.13 path) vs the curl-free client (dl.c), both with
    // zero-copy RX (zerocopy= in config.ini).
    bool go = true;
#ifdef DSPICO_DSWIFI
    go = run_net_sd_async(cfg, path, len, "curl SD", false, false, &rows[nrows++]);
    if (go)
        go = run_dl_net("dl net", path, len, true, &rows[nrows++]);
    if (go)
        go = run_net_sd_async(cfg, path, len, "dl SD", true, true, &rows[nrows++]);
    // A/B: main RAM bus priority to the ARM7 (EXMEMCNT bit 15), which copies
    // every received packet into main RAM while the ARM9 decrypts/writes SD.
    if (go)
    {
        REG_EXMEMCNT |= BIT(15);
        go = run_dl_net("dl net7", path, len, false, &rows[nrows++]);
        if (go)
            go = run_net_sd_async(cfg, path, len, "dl SD7", false, true, &rows[nrows++]);
        REG_EXMEMCNT &= ~BIT(15);
    }

#else
    go = run_net("HTTPS x1", NULL, path, len, 1, true, &rows[nrows++]);
    if (go)
        go = run_net_sd_async(cfg, path, len, "SD async", true, false, &rows[nrows++]);
#endif

    if (go && cfg->http_url[0])
        go = run_net("HTTP  x1", cfg->http_url, path, len, 1, false, &rows[nrows++]);

    // Summary
    consoleClear();
    log_printf("=== Summary (TCP_WND %s) ===\n", TCP_WND_LABEL);
    log_printf("RTT min/med  %lu/%lu ms\n", (unsigned long)rtt_min,
               (unsigned long)rtt_med);
    log_printf("CPU chacha   %lu KB/s\n", (unsigned long)chacha);
    log_printf("CPU aes-gcm  %lu KB/s\n", (unsigned long)gcm);
    log_printf("SD write     %lu KB/s (best)\n", (unsigned long)sd);
    log_printf("Test size    %lu KB\n\n", (unsigned long)(len / 1024));
    log_printf("test     KB/s lost wait  sd  cpu a7\n");
    for (int i = 0; i < nrows; i++)
    {
        uint32_t busy = rows[i].wait_pct + rows[i].sd_pct;
        log_printf("%-8s %4lu %4lu %3lu%% %2lu%% %3lu%%%2lu%s\n", rows[i].label,
                   (unsigned long)rows[i].steady_kbps,
                   (unsigned long)rows[i].rx_lost,
                   (unsigned long)rows[i].wait_pct,
                   (unsigned long)rows[i].sd_pct,
                   (unsigned long)(busy < 100 ? 100 - busy : 0),
                   (unsigned long)rows[i].a7_pct,
                   rows[i].ok ? "" : "F");
    }
    log_printf("wait: blocked on network\n"
               "cpu: TLS, curl, copies\n"
               "a7: ARM7 Wi-Fi handler load %%\n");
    for (int i = 0; i < nrows; i++)
        log_file_only("  %s: rx_queued %lu, rx_lost %lu\n", rows[i].label,
                      (unsigned long)rows[i].rx_queued,
                      (unsigned long)rows[i].rx_lost);
    if (rtt_med)
        log_printf("1-conn limit ~%lu KB/s\n",
                   (unsigned long)kbps(TCP_WND_BYTES, rtt_med));
}

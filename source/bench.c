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

static bool net_progress(void *ctx, uint64_t now, uint64_t total, uint32_t ms)
{
    (void)ctx;
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

static void stats_delta(net_row_t *row, const net_wifi_stats_t *a)
{
    net_wifi_stats_t b;
    net_wifi_stats(&b);
    row->rx_queued = b.rx_queued - a->rx_queued;
    row->rx_lost = b.rx_lost - a->rx_lost;
    if (b.tx_rejected != a->tx_rejected)
        log_printf("  TX rejected: %lu\n",
                   (unsigned long)(b.tx_rejected - a->tx_rejected));
}

static bool run_net(const char *label, const char *base, const char *path,
                    uint64_t len, int conns, net_row_t *row)
{
    log_printf("%s...\n", label);
    net_wifi_stats_t st;
    net_wifi_stats(&st);
    net_par_result_t r;
    prof_reset();
    if (conns == 1)
    {
        memcpy_stats_reset();
        g_memcpy_stats_on = 1;
        prof_sampling_start();
    }
    net_download_parallel(base, path, len, conns, net_progress, NULL, &r);
    if (conns == 1)
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
    // Same strategy as the real app will use: reserve the file, big buffer.
    setvbuf(f, NULL, _IOFBF, buf_size);
    if (!sd_preallocate(f, len))
        log_printf("  preallocation failed\n");

    net_wifi_stats_t st;
    net_wifi_stats(&st);
    net_result_t r;
    prof_reset();
    bool sample = buf_size == 32 * 1024;
    if (sample)
    {
        memcpy_stats_reset();
        g_memcpy_stats_on = 1;
        prof_sampling_start();
    }
    bool ok = net_download_range(path, 0, len - 1, f, net_progress, NULL, &r);
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
                             net_row_t *row)
{
    const char *label = "SD async";
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
    net_result_t r;
    prof_reset();
    bool ok = net_download_range_async(path, 0, len - 1, w, net_progress, NULL, &r);
    // Count the time to drain the ring: the download isn't done before that.
    uint64_t t0 = prof_ticks();
    uint64_t stall_ticks = sdw_stall_ticks(w);
    bool wok = sdw_close(w); // Drains the ring, then frees the writer
    uint32_t drain_ms = prof_ticks_to_ms(prof_ticks() - t0);
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
    log_printf("  ring full %lu ms, drain %lu ms\n",
               (unsigned long)prof_ticks_to_ms(stall_ticks),
               (unsigned long)drain_ms);
    if (!row->ok)
        log_printf("  FAILED: %s\n", r.error[0] ? r.error : "write/short/bad status");
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

    g_bench_cfg = cfg;
    net_row_t rows[7];
    int nrows = 0;
    // Parallel connections were shown to be slower; keep 1 and 2 only.
    const char *labels[2] = { "HTTPS x1", "HTTPS x2" };
    const int conns[2] = { 1, 2 };
    bool go = true;
    for (int i = 0; i < 2 && go; i++)
        go = run_net(labels[i], NULL, path, len, conns[i], &rows[nrows++]);

    if (go)
        go = run_net_sd(cfg, path, len, 32 * 1024, "SD 32K", &rows[nrows++]);
    if (go)
        go = run_net_sd(cfg, path, len, 256 * 1024, "SD 256K", &rows[nrows++]);
    if (go)
        go = run_net_sd_async(cfg, path, len, &rows[nrows++]);

    if (go && cfg->http_url[0])
        go = run_net("HTTP  x1", cfg->http_url, path, len, 1, &rows[nrows++]);

    // Summary
    consoleClear();
    log_printf("=== Summary (TCP_WND %s) ===\n", TCP_WND_LABEL);
    log_printf("RTT min/med  %lu/%lu ms\n", (unsigned long)rtt_min,
               (unsigned long)rtt_med);
    log_printf("CPU chacha   %lu KB/s\n", (unsigned long)chacha);
    log_printf("CPU aes-gcm  %lu KB/s\n", (unsigned long)gcm);
    log_printf("SD write     %lu KB/s (best)\n", (unsigned long)sd);
    log_printf("Test size    %lu KB\n\n", (unsigned long)(len / 1024));
    log_printf("test     KB/s lost wait  sd  cpu\n");
    for (int i = 0; i < nrows; i++)
    {
        uint32_t busy = rows[i].wait_pct + rows[i].sd_pct;
        log_printf("%-8s %4lu %4lu %3lu%% %2lu%% %3lu%%%s\n", rows[i].label,
                   (unsigned long)rows[i].steady_kbps,
                   (unsigned long)rows[i].rx_lost,
                   (unsigned long)rows[i].wait_pct,
                   (unsigned long)rows[i].sd_pct,
                   (unsigned long)(busy < 100 ? 100 - busy : 0),
                   rows[i].ok ? "" : " F");
    }
    log_printf("wait: blocked on network\n"
               "cpu: TLS, curl, copies\n");
    for (int i = 0; i < nrows; i++)
        log_file_only("  %s: rx_queued %lu, rx_lost %lu\n", rows[i].label,
                      (unsigned long)rows[i].rx_queued,
                      (unsigned long)rows[i].rx_lost);
    if (rtt_med)
        log_printf("1-conn limit ~%lu KB/s\n",
                   (unsigned long)kbps(TCP_WND_BYTES, rtt_med));
}

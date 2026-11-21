// SPDX-License-Identifier: MIT
#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "log.h"

// Provided by DSWiFi's lwIP port: 1 ms tick counter driven by a hardware timer.
extern uint32_t sys_now(void);

static const config_t *g_cfg;
static CURL *g_curl;
static char *g_ca_pem;
static size_t g_ca_len;

uint32_t net_now_ms(void)
{
    return sys_now();
}

void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool buf_append(buf_t *b, const char *data, size_t len)
{
    if (b->len + len + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + len + 1)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p)
            return false;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// CA loading
// ---------------------------------------------------------------------------

static bool load_ca(const config_t *cfg)
{
    char path[CFG_PATH_MAX + 16];
    config_path(cfg, "ca.pem", path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        log_printf("No ca.pem found\n");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024)
    {
        log_printf("ca.pem has bad size %ld\n", size);
        fclose(f);
        return false;
    }

    g_ca_pem = malloc(size + 1);
    if (!g_ca_pem || fread(g_ca_pem, 1, size, f) != (size_t)size)
    {
        log_printf("Can't read ca.pem\n");
        fclose(f);
        return false;
    }
    fclose(f);
    g_ca_pem[size] = '\0';
    g_ca_len = size;

    // Parse once here only to report how many certificates are usable and how
    // long parsing takes. curl parses the blob again for each new connection.
    uint32_t t0 = net_now_ms();
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    int ret = mbedtls_x509_crt_parse(&chain, (const unsigned char *)g_ca_pem,
                                     g_ca_len + 1);
    int count = 0;
    for (mbedtls_x509_crt *c = &chain; c != NULL && c->raw.len > 0; c = c->next)
        count++;
    mbedtls_x509_crt_free(&chain);

    log_printf("CA: %d certs, %d failed, %lu ms\n", count, ret < 0 ? -1 : ret,
               (unsigned long)(net_now_ms() - t0));

    if (ret < 0 || count == 0)
    {
        char err[100];
        mbedtls_strerror(ret, err, sizeof(err));
        log_printf("CA parse error: %s\n", err);
        return false;
    }
    return true;
}

bool net_init(const config_t *cfg)
{
    g_cfg = cfg;

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS)
    {
        log_printf("psa_crypto_init failed: %d\n", (int)ps);
        return false;
    }

    if (!load_ca(cfg) && !cfg->insecure)
    {
        log_printf("Put your root CA in romm/ca.pem\n"
                   "or set insecure=1 (test only)\n");
        return false;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
    {
        log_printf("curl_global_init failed\n");
        return false;
    }

    g_curl = curl_easy_init();
    if (!g_curl)
    {
        log_printf("curl_easy_init failed\n");
        return false;
    }

    log_file_only("%s\n", curl_version());
    return true;
}

void net_cleanup(void)
{
    if (g_curl)
        curl_easy_cleanup(g_curl);
    g_curl = NULL;
    curl_global_cleanup();
    free(g_ca_pem);
    g_ca_pem = NULL;
}

char *net_escape(const char *s)
{
    char *e = curl_easy_escape(g_curl, s, 0);
    if (!e)
        return NULL;
    char *copy = strdup(e);
    curl_free(e);
    return copy;
}

// ---------------------------------------------------------------------------
// Request plumbing
// ---------------------------------------------------------------------------

typedef struct
{
    CURL *h;
    buf_t *mem;
    FILE *fp;
    net_progress_fn cb;
    void *cb_ctx;
    uint32_t start_ms;
    uint64_t body_bytes;
    uint32_t first_byte_ms; // Absolute net_now_ms() of the first body byte
    bool write_failed;
    net_result_t *res;
} xfer_t;

static int debug_cb(CURL *h, curl_infotype type, char *data, size_t size,
                    void *userp)
{
    (void)h;
    (void)userp;

    // Only text and headers; never dump bodies (binary data and tokens).
    if (type == CURLINFO_TEXT)
    {
        log_file_only("* ");
        log_file_write(data, size);
    }
    else if (type == CURLINFO_HEADER_IN)
    {
        // curl delivers incoming headers one line at a time.
        if (size >= 11 && strncasecmp(data, "set-cookie:", 11) == 0)
            log_file_only("< Set-Cookie: <redacted>\r\n");
        else
        {
            log_file_only("< ");
            log_file_write(data, size);
        }
    }
    else if (type == CURLINFO_HEADER_OUT)
    {
        // Strip the Authorization header value from the log.
        const char *auth = strstr(data, "Authorization:");
        if (auth && (size_t)(auth - data) < size)
        {
            log_file_only("> ");
            log_file_write(data, auth - data);
            log_file_only("Authorization: <redacted>\r\n");
            const char *eol = strstr(auth, "\r\n");
            if (eol && (size_t)(eol + 2 - data) <= size)
                log_file_write(eol + 2, size - (eol + 2 - data));
        }
        else
        {
            log_file_only("> ");
            log_file_write(data, size);
        }
    }
    return 0;
}

static void capture_tls_info(xfer_t *x)
{
    if (x->res->tls_version)
        return;

    struct curl_tlssessioninfo *info = NULL;
    if (curl_easy_getinfo(x->h, CURLINFO_TLS_SSL_PTR, &info) != CURLE_OK || !info)
        return;
    if (info->backend != CURLSSLBACKEND_MBEDTLS || !info->internals)
        return;

    mbedtls_ssl_context *ssl = info->internals;
    x->res->tls_version = mbedtls_ssl_get_version(ssl);
    x->res->tls_cipher = mbedtls_ssl_get_ciphersuite(ssl);
}

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userp)
{
    xfer_t *x = userp;
    size_t len = size * nmemb;

    capture_tls_info(x);
    if (x->first_byte_ms == 0)
        x->first_byte_ms = net_now_ms() | 1; // Never 0 once set
    x->body_bytes += len;

    if (x->mem)
    {
        // JSON responses are kept in RAM; cap them to protect the heap.
        if (x->mem->len + len > 4 * 1024 * 1024 || !buf_append(x->mem, data, len))
        {
            x->write_failed = true;
            return 0;
        }
    }
    else if (x->fp)
    {
        if (fwrite(data, 1, len, x->fp) != len)
        {
            x->write_failed = true;
            return 0;
        }
    }
    return len;
}

static int xferinfo_cb(void *userp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal;
    (void)ulnow;
    (void)dlnow;

    xfer_t *x = userp;
    if (!x->cb)
        return 0;

    // Report decoded body bytes so file writes and progress agree.
    bool keep_going = x->cb(x->cb_ctx, x->body_bytes, (uint64_t)dltotal,
                            net_now_ms() - x->start_ms);
    return keep_going ? 0 : 1;
}

// Applies the options shared by every request to handle h. Returns the header
// list, which the caller frees after the transfer.
static struct curl_slist *setup_handle(CURL *h, const char *url, bool auth,
                                       bool compressed, const char *post_json,
                                       char *errbuf, xfer_t *x)
{
    x->h = h;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "dspico-romm/" POC_VERSION);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 64L * 1024);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);

    if (g_ca_pem)
    {
        struct curl_blob blob = {
            .data = g_ca_pem,
            .len = g_ca_len,
            .flags = CURL_BLOB_NOCOPY,
        };
        curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
    }

    if (g_cfg->insecure)
    {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // gzip for JSON only; ROM bodies are already dense and inflating costs CPU.
    if (compressed)
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");

    if (g_cfg->verbose)
    {
        curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, debug_cb);
    }

    struct curl_slist *headers = NULL;
    if (auth && g_cfg->token[0])
    {
        char auth_header[CFG_TOKEN_MAX + 32];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
                 g_cfg->token);
        headers = curl_slist_append(headers, auth_header); // curl copies it
    }
    if (post_json)
    {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, post_json);
    }
    headers = curl_slist_append(headers, "Accept: application/json, */*");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, x);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);

    return headers;
}

static void collect_info(CURL *h, net_result_t *res)
{
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res->status);

    curl_off_t t;
    if (curl_easy_getinfo(h, CURLINFO_CONNECT_TIME_T, &t) == CURLE_OK)
        res->connect_ms = (uint32_t)(t / 1000);
    if (curl_easy_getinfo(h, CURLINFO_APPCONNECT_TIME_T, &t) == CURLE_OK)
        res->tls_ms = (uint32_t)(t / 1000);
    if (curl_easy_getinfo(h, CURLINFO_STARTTRANSFER_TIME_T, &t) == CURLE_OK)
        res->ttfb_ms = (uint32_t)(t / 1000);
    if (curl_easy_getinfo(h, CURLINFO_SIZE_DOWNLOAD_T, &t) == CURLE_OK)
        res->wire_bytes = (uint64_t)t;
}

static bool perform(const char *path, const char *post_json, bool auth,
                    bool compressed, const char *range, xfer_t *x)
{
    net_result_t *res = x->res;
    memset(res, 0, sizeof(*res));

    if (!g_curl)
    {
        snprintf(res->error, sizeof(res->error), "net not initialised");
        return false;
    }

    char url[CFG_URL_MAX + 512];
    snprintf(url, sizeof(url), "%s%s", g_cfg->url, path);

    // curl_easy_reset keeps the connection cache, so keep-alive still works.
    curl_easy_reset(g_curl);
    struct curl_slist *headers = setup_handle(g_curl, url, auth, compressed,
                                              post_json, res->error, x);
    if (range)
        curl_easy_setopt(g_curl, CURLOPT_RANGE, range);

    x->start_ms = net_now_ms();
    CURLcode rc = curl_easy_perform(g_curl);
    res->total_ms = net_now_ms() - x->start_ms;

    curl_slist_free_all(headers);

    res->curl_code = rc;
    collect_info(g_curl, res);
    res->body_bytes = x->body_bytes;

    if (rc != CURLE_OK && res->error[0] == '\0')
        snprintf(res->error, sizeof(res->error), "%s", curl_easy_strerror(rc));
    if (x->write_failed && res->error[0] == '\0')
        snprintf(res->error, sizeof(res->error), "write failed");

    return rc == CURLE_OK && !x->write_failed;
}

bool net_get(const char *path, bool auth, buf_t *out, net_result_t *res)
{
    xfer_t x = { .mem = out, .res = res };
    return perform(path, NULL, auth, true, NULL, &x);
}

bool net_post_json(const char *path, const char *json, bool auth, buf_t *out,
                   net_result_t *res)
{
    xfer_t x = { .mem = out, .res = res };
    return perform(path, json, auth, true, NULL, &x);
}

bool net_download(const char *path, FILE *fp, net_progress_fn cb, void *ctx,
                  net_result_t *res)
{
    xfer_t x = { .fp = fp, .cb = cb, .cb_ctx = ctx, .res = res };
    return perform(path, NULL, true, false, NULL, &x);
}

void net_log_result(const char *label, const net_result_t *res)
{
    log_printf("%s: HTTP %ld, %lu ms\n", label, res->status,
               (unsigned long)res->total_ms);
    log_printf("  tcp %lu tls %lu ttfb %lu ms\n", (unsigned long)res->connect_ms,
               (unsigned long)res->tls_ms, (unsigned long)res->ttfb_ms);
    log_printf("  body %llu B, wire %llu B\n", (unsigned long long)res->body_bytes,
               (unsigned long long)res->wire_bytes);
    if (res->tls_version)
        log_printf("  %s %s\n", res->tls_version,
                   res->tls_cipher ? res->tls_cipher : "?");
    if (res->curl_code != 0)
        log_printf("  error %d: %s\n", res->curl_code, res->error);
}

// ---------------------------------------------------------------------------
// Parallel ranged download (speed test)
// ---------------------------------------------------------------------------

#define PAR_MAX_CONNS 8

bool net_download_parallel(const char *base_url, const char *path,
                           uint64_t length, int conns, net_progress_fn cb,
                           void *ctx, net_par_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->conns = conns;

    if (conns < 1 || conns > PAR_MAX_CONNS || length < (uint64_t)conns)
    {
        snprintf(out->error, sizeof(out->error), "bad parameters");
        return false;
    }

    char url[CFG_URL_MAX + 512];
    snprintf(url, sizeof(url), "%s%s", base_url ? base_url : g_cfg->url, path);

    CURLM *multi = curl_multi_init();
    CURL *h[PAR_MAX_CONNS] = { 0 };
    xfer_t x[PAR_MAX_CONNS];
    net_result_t r[PAR_MAX_CONNS];
    struct curl_slist *hdr[PAR_MAX_CONNS] = { 0 };

    uint64_t chunk = length / conns;
    for (int i = 0; i < conns; i++)
    {
        memset(&x[i], 0, sizeof(x[i]));
        memset(&r[i], 0, sizeof(r[i]));
        x[i].res = &r[i];

        h[i] = curl_easy_init();
        hdr[i] = setup_handle(h[i], url, true, false, NULL, r[i].error, &x[i]);
        // Each handle gets its own connection; no progress callback per handle.
        curl_easy_setopt(h[i], CURLOPT_NOPROGRESS, 1L);

        uint64_t first = chunk * i;
        uint64_t last = (i == conns - 1) ? length - 1 : first + chunk - 1;
        char range[48];
        snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)first,
                 (unsigned long long)last);
        curl_easy_setopt(h[i], CURLOPT_RANGE, range); // curl copies the string

        curl_multi_add_handle(multi, h[i]);
    }

    uint32_t start = net_now_ms();
    for (int i = 0; i < conns; i++)
        x[i].start_ms = start;

    uint32_t last_ui = start;
    int running = 1;
    bool aborted = false;
    while (running)
    {
        CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc != CURLM_OK)
        {
            snprintf(out->error, sizeof(out->error), "multi: %s",
                     curl_multi_strerror(mc));
            break;
        }
        if (running)
            curl_multi_poll(multi, NULL, 0, 50, NULL);

        uint32_t now = net_now_ms();
        if (cb && now - last_ui >= 500)
        {
            last_ui = now;
            uint64_t sum = 0;
            for (int i = 0; i < conns; i++)
                sum += x[i].body_bytes;
            if (!cb(ctx, sum, length, now - start))
            {
                aborted = true;
                snprintf(out->error, sizeof(out->error), "cancelled");
                break;
            }
        }
    }
    out->total_ms = net_now_ms() - start;

    int done_ok = 0;
    CURLMsg *msg;
    int queued;
    while ((msg = curl_multi_info_read(multi, &queued)))
    {
        if (msg->msg != CURLMSG_DONE)
            continue;
        if (msg->data.result == CURLE_OK)
            done_ok++;
        else if (out->error[0] == '\0')
            snprintf(out->error, sizeof(out->error), "%s",
                     curl_easy_strerror(msg->data.result));
    }

    for (int i = 0; i < conns; i++)
    {
        long status = 0;
        curl_easy_getinfo(h[i], CURLINFO_RESPONSE_CODE, &status);
        if (status != 206 && out->error[0] == '\0')
            snprintf(out->error, sizeof(out->error), "HTTP %ld (want 206)", status);
        if (i == 0)
            out->status = status;

        out->bytes += x[i].body_bytes;
        if (x[i].first_byte_ms)
        {
            uint32_t setup = x[i].first_byte_ms - start;
            if (setup > out->setup_ms)
                out->setup_ms = setup;
        }
        if (!out->tls_cipher && r[i].tls_cipher)
            out->tls_cipher = r[i].tls_cipher;

        curl_multi_remove_handle(multi, h[i]);
        curl_easy_cleanup(h[i]);
        curl_slist_free_all(hdr[i]);
    }
    curl_multi_cleanup(multi);

    out->ok = !aborted && done_ok == conns && out->bytes == length &&
              out->error[0] == '\0';
    return out->ok;
}

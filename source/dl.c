// SPDX-License-Identifier: MIT
//
// Minimal HTTPS/1.1 download client on Mbed TLS (see dl.h).

#include "dl.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "log.h"

#ifdef __NDS__
#include <nds.h>
#define DL_YIELD() cothread_yield()
#else
#define DL_YIELD() ((void)0)
#endif

// Yield to other cothreads after each TLS record (see the body loop).
bool dl_yield_per_record = true;
#include "sdwriter.h"
#include "tlsrx.h"

// Decrypt TLS records ourselves, straight from the network buffers (tlsrx.h).
bool dl_zero_copy_tls = true;

// DSWiFi lwIP patch (patches/dswifi-recv-zc.patch)
ssize_t lwip_recv_zc(int s, size_t max,
                     size_t (*fn)(void *ctx, const void *data, size_t len), void *ctx);

#define DL_TIMEOUT_MS   30000 // No data for this long: give up (like curl's low-speed limit)
#define DL_POLL_MS      250   // Progress callback / cancel check interval while waiting
#define DL_HDR_MAX      8192
#define DL_DISCARD_SIZE TLSRX_MAX_PLAIN // Also the bounce buffer for tlsrx

typedef struct
{
    bool tls_ready;          // Config, RNG and CA chain set up
    bool connected;
    int fd;
    char host[CFG_URL_MAX];
    char port[8];
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    uint8_t srv_secret[32];  // TLS 1.3 server application traffic secret
    bool have_secret;
} dl_state_t;

static dl_state_t g_dl;

// Ciphers in order of preference. ChaCha20-Poly1305 first: the ARM9 has no
// AES-GCM acceleration (measured 2.9 MB/s ChaCha20-Poly1305 vs 0.19 MB/s
// AES-128-GCM), and Go's TLS stack (Traefik) honours the client's order when
// the client has no AES hardware preference.
static const int g_ciphers[] = {
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
    MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    0
};

static void set_error(net_result_t *res, const char *what, int ret)
{
    char e[96] = "";
    if (ret < 0)
        mbedtls_strerror(ret, e, sizeof(e));
    snprintf(res->error, sizeof(res->error), "%s%s%s", what, e[0] ? ": " : "", e);
}

// --- Socket BIO (non-blocking; waiting happens in wait_fd) ------------------

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int n = send(fd, buf, len, MSG_DONTWAIT);
    if (n >= 0)
        return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int n = recv(fd, buf, len, MSG_DONTWAIT);
    if (n > 0)
        return n;
    if (n == 0)
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY; // EOF
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

typedef struct
{
    net_progress_fn cb;
    void *ctx;
    uint64_t done, total;
    uint32_t t0;
    uint32_t last_data;
    uint32_t last_cb;
    bool cancelled;
} progress_t;

// Waits until the socket is ready for want (WANT_READ / WANT_WRITE). Calls
// the progress callback every DL_POLL_MS. Returns false on timeout or cancel.
static bool wait_fd(int want, progress_t *p, net_result_t *res)
{
    struct pollfd pfd = {
        .fd = g_dl.fd,
        .events = want == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN,
    };
    poll(&pfd, 1, DL_POLL_MS);

    uint32_t now = net_now_ms();
    if (now - p->last_data > DL_TIMEOUT_MS)
    {
        snprintf(res->error, sizeof(res->error), "Timeout was reached");
        res->curl_code = 28; // CURLE_OPERATION_TIMEDOUT
        return false;
    }

    // The callback prints to the console: call it at most every 100 ms.
    if (now - p->last_cb < 100)
        return true;
    p->last_cb = now;
    if (p->cb && !p->cb(p->ctx, p->done, p->total, now - p->t0))
    {
        p->cancelled = true;
        snprintf(res->error, sizeof(res->error), "cancelled");
        res->curl_code = 42; // Same code curl uses (CURLE_ABORTED_BY_CALLBACK)
        return false;
    }
    return true;
}

// --- Connection --------------------------------------------------------------

void dl_close(void)
{
    if (g_dl.connected)
    {
        mbedtls_ssl_close_notify(&g_dl.ssl);
        mbedtls_ssl_free(&g_dl.ssl);
        close(g_dl.fd);
        g_dl.connected = false;
    }
}

static bool tls_setup(net_result_t *res)
{
    if (g_dl.tls_ready)
        return true;

    const config_t *cfg = net_config();

    // "https://host[:port]" without path (config.c strips trailing '/')
    const char *u = cfg->url;
    if (strncasecmp(u, "https://", 8) != 0)
    {
        snprintf(res->error, sizeof(res->error), "url must start with https://");
        return false;
    }
    u += 8;
    size_t hl = strcspn(u, ":/");
    if (hl == 0 || hl >= sizeof(g_dl.host))
    {
        snprintf(res->error, sizeof(res->error), "bad host in url");
        return false;
    }
    memcpy(g_dl.host, u, hl);
    g_dl.host[hl] = '\0';
    if (u[hl] == ':')
        snprintf(g_dl.port, sizeof(g_dl.port), "%.*s", (int)strcspn(u + hl + 1, "/"),
                 u + hl + 1);
    else
        snprintf(g_dl.port, sizeof(g_dl.port), "443");

    mbedtls_entropy_init(&g_dl.entropy);
    mbedtls_ctr_drbg_init(&g_dl.drbg);
    mbedtls_x509_crt_init(&g_dl.ca);
    mbedtls_ssl_config_init(&g_dl.conf);

    int ret = mbedtls_ctr_drbg_seed(&g_dl.drbg, mbedtls_entropy_func, &g_dl.entropy,
                                    (const unsigned char *)"dspico-romm-dl", 14);
    if (ret != 0)
    {
        set_error(res, "RNG seed", ret);
        return false;
    }

    size_t ca_len;
    const char *ca = net_ca_pem(&ca_len);
    if (ca)
    {
        ret = mbedtls_x509_crt_parse(&g_dl.ca, (const unsigned char *)ca, ca_len + 1);
        if (ret < 0)
        {
            set_error(res, "CA parse", ret);
            return false;
        }
    }

    ret = mbedtls_ssl_config_defaults(&g_dl.conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        set_error(res, "TLS config", ret);
        return false;
    }
    mbedtls_ssl_conf_rng(&g_dl.conf, mbedtls_ctr_drbg_random, &g_dl.drbg);
    mbedtls_ssl_conf_ca_chain(&g_dl.conf, &g_dl.ca, NULL);
    mbedtls_ssl_conf_authmode(&g_dl.conf, cfg->insecure ? MBEDTLS_SSL_VERIFY_NONE
                                                        : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ciphersuites(&g_dl.conf, g_ciphers);
    mbedtls_ssl_conf_min_tls_version(&g_dl.conf, MBEDTLS_SSL_VERSION_TLS1_2);

    g_dl.tls_ready = true;
    return true;
}

// Keeps the server application traffic secret for tlsrx (TLS 1.3 only).
static void export_keys_cb(void *ctx, mbedtls_ssl_key_export_type type,
                           const unsigned char *secret, size_t secret_len,
                           const unsigned char client_random[32],
                           const unsigned char server_random[32],
                           mbedtls_tls_prf_types tls_prf_type)
{
    (void)client_random;
    (void)server_random;
    (void)tls_prf_type;
    dl_state_t *d = ctx;
    if (type == MBEDTLS_SSL_KEY_EXPORT_TLS1_3_SERVER_APPLICATION_TRAFFIC_SECRET &&
        secret_len == sizeof(d->srv_secret))
    {
        memcpy(d->srv_secret, secret, secret_len);
        d->have_secret = true;
    }
}

static bool dl_connect(progress_t *p, net_result_t *res)
{
    if (g_dl.connected)
        return true;
    if (!tls_setup(res))
        return false;

    uint32_t t0 = net_now_ms();

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *ai = NULL;
    if (getaddrinfo(g_dl.host, g_dl.port, &hints, &ai) != 0 || !ai)
    {
        snprintf(res->error, sizeof(res->error), "Could not resolve hostname");
        res->curl_code = 6;
        return false;
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
    {
        freeaddrinfo(ai);
        snprintf(res->error, sizeof(res->error), "socket() failed");
        return false;
    }
    int ret = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (ret != 0)
    {
        close(fd);
        snprintf(res->error, sizeof(res->error), "connect() failed (%d)", errno);
        res->curl_code = 7;
        return false;
    }
    res->connect_ms = net_now_ms() - t0;

    g_dl.fd = fd;
    mbedtls_ssl_init(&g_dl.ssl);
    ret = mbedtls_ssl_setup(&g_dl.ssl, &g_dl.conf);
    if (ret == 0)
        ret = mbedtls_ssl_set_hostname(&g_dl.ssl, g_dl.host);
    if (ret != 0)
    {
        mbedtls_ssl_free(&g_dl.ssl);
        close(fd);
        set_error(res, "TLS setup", ret);
        return false;
    }
    mbedtls_ssl_set_bio(&g_dl.ssl, &g_dl.fd, bio_send, bio_recv, NULL);
    g_dl.have_secret = false;
    mbedtls_ssl_set_export_keys_cb(&g_dl.ssl, export_keys_cb, &g_dl);
    g_dl.connected = true;

    p->last_data = net_now_ms();
    while ((ret = mbedtls_ssl_handshake(&g_dl.ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            set_error(res, "TLS handshake", ret);
            res->curl_code = 35;
            dl_close();
            return false;
        }
        if (!wait_fd(ret, p, res))
        {
            dl_close();
            return false;
        }
    }
    res->tls_ms = net_now_ms() - t0;
    return true;
}

// Reads decrypted data. Returns >0 bytes, 0 on orderly EOF, <0 on error
// (res->error set). Waits as needed.
static int tls_read(unsigned char *buf, size_t len, progress_t *p, net_result_t *res)
{
    while (1)
    {
        int n = mbedtls_ssl_read(&g_dl.ssl, buf, len);
        if (n > 0)
        {
            p->last_data = net_now_ms();
            return n;
        }
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (!wait_fd(n, p, res))
                return -1;
            continue;
        }
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue; // TLS 1.3 post-handshake message, not data
#endif
        set_error(res, "TLS read", n);
        res->curl_code = 56;
        return -1;
    }
}

static bool tls_write_all(const unsigned char *buf, size_t len, progress_t *p,
                          net_result_t *res)
{
    while (len > 0)
    {
        int n = mbedtls_ssl_write(&g_dl.ssl, buf, len);
        if (n > 0)
        {
            buf += n;
            len -= n;
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (!wait_fd(n, p, res))
                return false;
            continue;
        }
        set_error(res, "TLS write", n);
        res->curl_code = 55;
        return false;
    }
    return true;
}

// --- HTTP ----------------------------------------------------------------------

// Finds header `name` (case-insensitive) in the header block; copies its
// trimmed value into out. Returns false if absent.
static bool header_value(const char *hdrs, const char *name, char *out, size_t out_len)
{
    size_t nl = strlen(name);
    for (const char *l = strstr(hdrs, "\r\n"); l; l = strstr(l, "\r\n"))
    {
        l += 2;
        if (strncasecmp(l, name, nl) == 0 && l[nl] == ':')
        {
            const char *v = l + nl + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vl = strcspn(v, "\r\n");
            if (vl >= out_len)
                vl = out_len - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return true;
        }
    }
    return false;
}

// True when the rest of the stream can be decrypted by tlsrx: TLS 1.3 with
// ChaCha20-Poly1305, and Mbed TLS holds no unread data (it reads exactly one
// record at a time, so the socket is then positioned at a record boundary).
static bool zc_possible(void)
{
    mbedtls_ssl_context *ssl = &g_dl.ssl;
    return dl_zero_copy_tls && g_dl.have_secret &&
           mbedtls_ssl_get_ciphersuite_id_from_ssl(ssl) ==
               MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256 &&
           mbedtls_ssl_get_bytes_avail(ssl) == 0 &&
           ssl->MBEDTLS_PRIVATE(in_left) == 0;
}

static uint64_t be64(const unsigned char *b)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | b[i];
    return v;
}

// Receives body bytes [*got, body_len) with tlsrx. Afterwards Mbed TLS's
// record counter is updated, so the connection can be reused.
static bool zc_body(sdwriter_t *sdw, unsigned char *bounce, uint64_t *got,
                    uint64_t body_len, progress_t *p, net_result_t *res)
{
    unsigned char *ctr = g_dl.ssl.MBEDTLS_PRIVATE(in_ctr);
    static tlsrx_t z;
    if (!tlsrx_init(&z, g_dl.srv_secret, be64(ctr)))
    {
        snprintf(res->error, sizeof(res->error), "tlsrx init failed");
        return false;
    }

    bool ok = false;
    bool in_ring = false;
    while (*got < body_len)
    {
        if (!z.dst)
        {
            size_t avail = 0;
            unsigned char *r = NULL;
            if (sdw && !(r = sdw_reserve(sdw, &avail)))
            {
                snprintf(res->error, sizeof(res->error), "SD write failed");
                goto out;
            }
            in_ring = r && avail >= TLSRX_MAX_PLAIN;
            z.dst = in_ring ? r : bounce;
        }

        ssize_t n = lwip_recv_zc(g_dl.fd, SIZE_MAX, tlsrx_feed, &z);
        if (n < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                if (!wait_fd(MBEDTLS_ERR_SSL_WANT_READ, p, res))
                    goto out;
                continue;
            }
            snprintf(res->error, sizeof(res->error),
                     errno == ENOTCONN ? "connection closed early" : "recv failed (%d)",
                     errno);
            goto out;
        }
        if (n > 0)
            p->last_data = net_now_ms();
        if (z.error)
        {
            snprintf(res->error, sizeof(res->error), "%s", z.error);
            res->curl_code = 56;
            goto out;
        }
        if (!z.rec_ready)
            continue;
        z.rec_ready = false;

        if (z.rec_type == 23) // application_data
        {
            if (z.rec_len > body_len - *got)
            {
                snprintf(res->error, sizeof(res->error), "more data than Content-Length");
                goto out;
            }
            if (sdw)
            {
                if (in_ring)
                    sdw_commit(sdw, z.rec_len);
                else if (!sdw_write(sdw, bounce, z.rec_len))
                {
                    snprintf(res->error, sizeof(res->error), "SD write failed");
                    goto out;
                }
            }
            *got += z.rec_len;
            p->done = *got;
            z.dst = NULL;
            if (dl_yield_per_record)
                DL_YIELD();
        }
        else if (z.rec_type == 22) // handshake, e.g. NewSessionTicket: ignore
        {
            if (z.rec_len > 0 && z.dst[0] != 4)
            {
                snprintf(res->error, sizeof(res->error),
                         "TLS handshake message %u during download", z.dst[0]);
                goto out;
            }
        }
        else if (z.rec_type == 21) // alert
        {
            snprintf(res->error, sizeof(res->error), "TLS alert %u/%u",
                     z.rec_len > 0 ? z.dst[0] : 0, z.rec_len > 1 ? z.dst[1] : 0);
            goto out;
        }
        else
        {
            snprintf(res->error, sizeof(res->error), "TLS record type %u", z.rec_type);
            goto out;
        }
    }
    ok = true;

out:
    // Hand the record counter back to Mbed TLS (big-endian)
    for (int i = 0; i < 8; i++)
        ctr[i] = (unsigned char)(z.seq >> (56 - 8 * i));
    tlsrx_free(&z);
    return ok;
}

static bool request_once(const char *path, uint64_t first, uint64_t last,
                         sdwriter_t *sdw, progress_t *p, net_result_t *res,
                         bool *retry)
{
    const config_t *cfg = net_config();
    bool reused = g_dl.connected;
    *retry = false;

    if (!dl_connect(p, res))
        return false;

    static char req[1024 + CFG_TOKEN_MAX];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: dspico-romm/" POC_VERSION "\r\n"
                      "Accept: */*\r\n"
                      "Range: bytes=%llu-%llu\r\n"
                      "%s%s%s"
                      "\r\n",
                      path, g_dl.host, (unsigned long long)first,
                      (unsigned long long)last,
                      cfg->token[0] ? "Authorization: Bearer " : "",
                      cfg->token[0] ? cfg->token : "",
                      cfg->token[0] ? "\r\n" : "");
    if (rl <= 0 || rl >= (int)sizeof(req))
    {
        snprintf(res->error, sizeof(res->error), "request too long");
        return false;
    }

    uint32_t t_req = net_now_ms();
    if (!tls_write_all((const unsigned char *)req, rl, p, res))
    {
        dl_close();
        *retry = reused && !p->cancelled;
        return false;
    }

    // Response headers. Read in small pieces so that little body data lands
    // in the header buffer (it is moved to the destination afterwards).
    static char hdr[DL_HDR_MAX + 1];
    size_t hl = 0;
    char *end = NULL;
    while (!end)
    {
        if (hl >= DL_HDR_MAX)
        {
            snprintf(res->error, sizeof(res->error), "response headers too long");
            dl_close();
            return false;
        }
        size_t want = DL_HDR_MAX - hl < 512 ? DL_HDR_MAX - hl : 512;
        int n = tls_read((unsigned char *)hdr + hl, want, p, res);
        if (n <= 0)
        {
            if (n == 0)
                snprintf(res->error, sizeof(res->error), "connection closed");
            dl_close();
            // A reused keep-alive connection may have been closed by the
            // server while idle: try once more on a new connection.
            *retry = reused && hl == 0 && !p->cancelled;
            return false;
        }
        if (hl == 0)
            res->ttfb_ms = net_now_ms() - t_req;
        hl += n;
        hdr[hl] = '\0';
        end = strstr(hdr, "\r\n\r\n");
    }
    size_t hdr_len = (end - hdr) + 4;
    end[2] = '\0'; // Keep the last header's CRLF for header_value()

    int major, minor, status;
    if (sscanf(hdr, "HTTP/%d.%d %d", &major, &minor, &status) != 3)
    {
        snprintf(res->error, sizeof(res->error), "bad status line");
        dl_close();
        return false;
    }
    res->status = status;

    char v[64];
    bool keep_alive = !(header_value(hdr, "Connection", v, sizeof(v)) &&
                        strcasecmp(v, "close") == 0);
    if (header_value(hdr, "Transfer-Encoding", v, sizeof(v)) &&
        strcasecmp(v, "identity") != 0)
    {
        snprintf(res->error, sizeof(res->error), "unsupported Transfer-Encoding %s", v);
        dl_close();
        return false;
    }
    uint64_t body_len = UINT64_MAX; // Until EOF
    if (header_value(hdr, "Content-Length", v, sizeof(v)))
        body_len = strtoull(v, NULL, 10);
    if (body_len == UINT64_MAX)
        keep_alive = false;
    p->total = body_len == UINT64_MAX ? 0 : body_len;

    // Error statuses: read and drop the (small) body, report failure.
    bool ok_status = status == 200 || status == 206;
    if (!ok_status)
        sdw = NULL;

    // Body bytes that came with the headers
    uint64_t got = 0;
    size_t extra = hl - hdr_len;
    if (extra > body_len)
        extra = body_len;
    while (extra > 0)
    {
        size_t avail = extra;
        void *dst = NULL;
        if (sdw && !(dst = sdw_reserve(sdw, &avail)))
            goto write_failed;
        if (avail > extra)
            avail = extra;
        if (dst)
        {
            memcpy(dst, hdr + hdr_len + got, avail);
            sdw_commit(sdw, avail);
        }
        got += avail;
        extra -= avail;
    }

    static unsigned char *discard;
    if (!discard)
        discard = malloc(DL_DISCARD_SIZE);
    if (!discard)
    {
        snprintf(res->error, sizeof(res->error), "out of memory");
        dl_close();
        return false;
    }

    // The body: Mbed TLS decrypts into its record buffer and copies straight
    // into the writer's ring.
    while (got < body_len)
    {
        // Rest of the body without Mbed TLS's copies (see tlsrx.h)
        if (body_len != UINT64_MAX && zc_possible())
        {
            res->zero_copy_tls = true;
            if (!zc_body(sdw, discard, &got, body_len, p, res))
            {
                dl_close();
                return false;
            }
            break;
        }

        size_t avail = DL_DISCARD_SIZE;
        unsigned char *dst = discard;
        if (sdw && !(dst = sdw_reserve(sdw, &avail)))
            goto write_failed;
        if (avail > body_len - got)
            avail = body_len - got;

        int n = tls_read(dst, avail, p, res);
        if (n < 0)
        {
            dl_close();
            return false;
        }
        if (n == 0)
        {
            if (body_len == UINT64_MAX)
                break; // Body delimited by EOF
            snprintf(res->error, sizeof(res->error), "connection closed early");
            dl_close();
            return false;
        }
        if (sdw)
            sdw_commit(sdw, n);
        got += n;
        p->done = got;

        // Cothreads only switch when one yields. While records are buffered,
        // this loop never waits, so lwIP's thread would not process incoming
        // segments (and send ACKs / window updates) until the backlog is
        // decrypted: the sender stalls. Let it run after every record.
        if (dl_yield_per_record)
            DL_YIELD();
    }
    res->body_bytes = got;
    res->wire_bytes = got;

    if (!keep_alive || body_len == UINT64_MAX)
        dl_close();

    if (!ok_status)
    {
        snprintf(res->error, sizeof(res->error), "HTTP %d", status);
        return false;
    }
    return true;

write_failed:
    snprintf(res->error, sizeof(res->error), "SD write failed");
    dl_close();
    return false;
}

bool dl_get_range(const char *path, uint64_t first, uint64_t last,
                  sdwriter_t *sdw, net_progress_fn cb, void *ctx,
                  net_result_t *res)
{
    memset(res, 0, sizeof(*res));
    progress_t p = { .cb = cb, .ctx = ctx, .total = last - first + 1 };
    p.t0 = net_now_ms();
    p.last_data = p.t0;

    bool retry;
    bool ok = request_once(path, first, last, sdw, &p, res, &retry);
    if (!ok && retry)
    {
        memset(res, 0, sizeof(*res));
        ok = request_once(path, first, last, sdw, &p, res, &retry);
    }

    res->total_ms = net_now_ms() - p.t0;
    if (g_dl.connected)
    {
        res->tls_version = mbedtls_ssl_get_version(&g_dl.ssl);
        res->tls_cipher = mbedtls_ssl_get_ciphersuite(&g_dl.ssl);
    }
    return ok;
}

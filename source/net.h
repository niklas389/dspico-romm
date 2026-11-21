// SPDX-License-Identifier: MIT
#ifndef ROMM_NET_H
#define ROMM_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"

typedef struct
{
    char *data; // Always NUL-terminated when non-NULL
    size_t len;
    size_t cap;
} buf_t;

void buf_free(buf_t *b);

typedef struct
{
    int curl_code;          // CURLcode, 0 on success
    long status;            // HTTP status, 0 if no response
    char error[256];        // curl error buffer
    uint32_t connect_ms;    // TCP connect (0 when the connection was reused)
    uint32_t tls_ms;        // Time until TLS handshake done (0 when reused)
    uint32_t ttfb_ms;       // Time to first byte
    uint32_t total_ms;      // Whole request, measured locally
    uint64_t body_bytes;    // Body bytes delivered to the app (decoded)
    uint64_t wire_bytes;    // Bytes received on the wire (CURLINFO_SIZE_DOWNLOAD_T)
    const char *tls_version;
    const char *tls_cipher;
} net_result_t;

// Called during downloads. Return false to abort the transfer.
typedef bool (*net_progress_fn)(void *ctx, uint64_t now, uint64_t total,
                                uint32_t elapsed_ms);

// Loads the CA file (romm/ca.pem), initialises PSA crypto and libcurl.
bool net_init(const config_t *cfg);
void net_cleanup(void);

// Milliseconds since boot (lwIP 1 ms timer, wraps after ~49 days).
uint32_t net_now_ms(void);

// GET <url><path> into memory. Adds the Bearer token when auth is true.
bool net_get(const char *path, bool auth, buf_t *out, net_result_t *res);

// POST a JSON body to <url><path>, response into memory.
bool net_post_json(const char *path, const char *json, bool auth, buf_t *out,
                   net_result_t *res);

// GET <url><path> streaming the body into fp (fp may be NULL to discard).
bool net_download(const char *path, FILE *fp, net_progress_fn cb, void *ctx,
                  net_result_t *res);

// URL-encodes a path segment. The returned string must be freed with free().
char *net_escape(const char *s);

void net_log_result(const char *label, const net_result_t *res);

#endif

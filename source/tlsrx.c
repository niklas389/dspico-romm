// SPDX-License-Identifier: MIT
//
// TLS 1.3 record decryption from network buffers (see tlsrx.h).

#include "tlsrx.h"

#include <string.h>

#include <mbedtls/md.h>

// HKDF-Expand-Label(secret, label, "", len) with SHA-256 (RFC 8446 7.1), for
// len <= 32 (a single HMAC block).
static bool hkdf_expand_label(const uint8_t secret[32], const char *label,
                              uint8_t *out, size_t len)
{
    uint8_t info[2 + 1 + 6 + 16 + 1 + 1];
    size_t ll = strlen(label);
    if (ll > 16 || len > 32)
        return false;

    size_t n = 0;
    info[n++] = 0;
    info[n++] = (uint8_t)len;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = 0;    // Empty context
    info[n++] = 0x01; // HKDF-Expand block counter T(1)

    uint8_t t[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, secret, 32, info, n, t) != 0)
        return false;
    memcpy(out, t, len);
    memset(t, 0, sizeof(t));
    return true;
}

bool tlsrx_init(tlsrx_t *z, const uint8_t secret[32], uint64_t seq)
{
    memset(z, 0, sizeof(*z));
    uint8_t key[32];
    if (!hkdf_expand_label(secret, "key", key, 32) ||
        !hkdf_expand_label(secret, "iv", z->iv, 12))
        return false;

    mbedtls_chachapoly_init(&z->cp);
    int ret = mbedtls_chachapoly_setkey(&z->cp, key);
    memset(key, 0, sizeof(key));
    z->seq = seq;
    return ret == 0;
}

void tlsrx_free(tlsrx_t *z)
{
    mbedtls_chachapoly_free(&z->cp);
    memset(z->iv, 0, sizeof(z->iv));
}

static bool start_record(tlsrx_t *z)
{
    // TLSCiphertext: opaque_type 23, legacy_record_version 0x0303, length
    if (z->hdr[0] != 23 || z->hdr[1] != 3 || z->hdr[2] != 3)
    {
        z->error = "unexpected TLS record header";
        return false;
    }
    size_t len = ((size_t)z->hdr[3] << 8) | z->hdr[4];
    if (len < 17 || len > 16384 + 256)
    {
        z->error = "bad TLS record length";
        return false;
    }
    if (!z->dst)
    {
        z->error = "no destination buffer";
        return false;
    }

    // Per-record nonce: IV XOR 64-bit big-endian sequence number
    uint8_t nonce[12];
    memcpy(nonce, z->iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] ^= (uint8_t)(z->seq >> (56 - 8 * i));

    if (mbedtls_chachapoly_starts(&z->cp, nonce, MBEDTLS_CHACHAPOLY_DECRYPT) != 0 ||
        mbedtls_chachapoly_update_aad(&z->cp, z->hdr, 5) != 0)
    {
        z->error = "chachapoly start failed";
        return false;
    }

    z->body_len = len - 16;
    z->body_done = 0;
    z->tag_n = 0;
    z->state = 1;
    return true;
}

static void finish_record(tlsrx_t *z)
{
    uint8_t calc[16];
    if (mbedtls_chachapoly_finish(&z->cp, calc) != 0)
    {
        z->error = "chachapoly finish failed";
        return;
    }
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= calc[i] ^ z->tag[i];
    if (diff != 0)
    {
        z->error = "TLS record authentication failed";
        return;
    }
    z->seq++;

    // TLSInnerPlaintext: content, type (non-zero), zero padding
    size_t i = z->body_len;
    while (i > 0 && z->dst[i - 1] == 0)
        i--;
    if (i == 0)
    {
        z->error = "TLS record without content type";
        return;
    }
    z->rec_type = z->dst[i - 1];
    z->rec_len = i - 1;
    z->rec_ready = true;
    z->state = 0;
    z->hdr_n = 0;
}

size_t tlsrx_feed(void *ctx, const void *data, size_t len)
{
    tlsrx_t *z = ctx;
    const uint8_t *src = data;
    size_t used = 0;

    while (used < len && !z->error && !z->rec_ready)
    {
        if (z->state == 0)
        {
            while (z->hdr_n < 5 && used < len)
                z->hdr[z->hdr_n++] = src[used++];
            if (z->hdr_n == 5 && !start_record(z))
                break;
        }
        else if (z->state == 1)
        {
            size_t n = z->body_len - z->body_done;
            if (n > len - used)
                n = len - used;
            if (mbedtls_chachapoly_update(&z->cp, n, src + used,
                                          z->dst + z->body_done) != 0)
            {
                z->error = "chachapoly update failed";
                break;
            }
            z->body_done += n;
            used += n;
            if (z->body_done == z->body_len)
                z->state = 2;
        }
        else
        {
            while (z->tag_n < 16 && used < len)
                z->tag[z->tag_n++] = src[used++];
            if (z->tag_n == 16)
                finish_record(z);
        }
    }
    return used;
}

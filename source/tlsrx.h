// SPDX-License-Identifier: MIT
#ifndef ROMM_TLSRX_H
#define ROMM_TLSRX_H

// Receive side of a TLS 1.3 TLS_CHACHA20_POLY1305_SHA256 connection, taken
// over from Mbed TLS after the handshake (see dl.c).
//
// Records are decrypted straight from the network buffers into the
// destination (the SD writer's ring): the only pass over the data after
// the Wi-Fi driver is the decryption itself. Mbed TLS would first copy each
// record into its own buffer, decrypt it there and copy it out again.
//
// Plaintext is written to dst before the record's tag has been checked; the
// caller must only use (commit) it once rec_ready is set, which happens only
// for records whose tag verified.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mbedtls/chachapoly.h>

// Largest TLS 1.3 ciphertext body (RFC 8446 5.2) minus the tag: the space
// dst must have for any record.
#define TLSRX_MAX_PLAIN (16384 + 256 - 16)

typedef struct
{
    mbedtls_chachapoly_context cp;
    uint8_t iv[12];
    uint64_t seq;

    // Current record
    int state;            // 0 header, 1 body, 2 tag
    uint8_t hdr[5];
    size_t hdr_n;
    size_t body_len;      // Ciphertext without tag (inner plaintext length)
    size_t body_done;
    uint8_t tag[16];
    size_t tag_n;

    // Set by the caller before feeding: at least TLSRX_MAX_PLAIN bytes.
    uint8_t *dst;

    // Result of the last completed record (caller clears rec_ready)
    bool rec_ready;
    uint8_t rec_type;     // Inner content type (23 = application data)
    size_t rec_len;       // Plaintext length without type and padding

    const char *error;    // Non-NULL: the stream is unusable
} tlsrx_t;

// Derives key and IV from the server application traffic secret (SHA-256,
// 32 bytes). seq is the number of records already received with it.
bool tlsrx_init(tlsrx_t *z, const uint8_t secret[32], uint64_t seq);
void tlsrx_free(tlsrx_t *z);

// Stream input, in the form lwip_recv_zc() expects: consumes bytes and
// returns how many. Stops right after a record has been completed
// (rec_ready) or on error, so the caller can act before the next record.
size_t tlsrx_feed(void *ctx, const void *data, size_t len);

#endif

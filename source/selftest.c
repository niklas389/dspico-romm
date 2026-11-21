// SPDX-License-Identifier: MIT
//
// Startup self-test of the replaced ChaCha20-Poly1305 code (RFC 8439 §2.8.2)
// at an aligned and at a misaligned address (TLS decrypts at in_buf + 13).

#include <stdbool.h>
#include <string.h>

#include <mbedtls/chachapoly.h>

static const unsigned char pt[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one "
    "tip for the future, sunscreen would be it.";

static const unsigned char ct_head[16] = {
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
    0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
};
static const unsigned char tag_ref[16] = {
    0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
    0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91,
};

static bool run_at(unsigned char *buf, size_t len)
{
    unsigned char key[32], tag[16];
    for (int i = 0; i < 32; i++)
        key[i] = 0x80 + i;
    const unsigned char nonce[12] = { 0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43,
                                      0x44, 0x45, 0x46, 0x47 };
    const unsigned char aad[12] = { 0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
                                    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7 };

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    bool ok = mbedtls_chachapoly_setkey(&ctx, key) == 0;

    memcpy(buf, pt, len);
    ok = ok && mbedtls_chachapoly_encrypt_and_tag(&ctx, len, nonce, aad,
                                                  sizeof(aad), buf, buf, tag) == 0;
    ok = ok && memcmp(buf, ct_head, 16) == 0 && memcmp(tag, tag_ref, 16) == 0;
    ok = ok && mbedtls_chachapoly_auth_decrypt(&ctx, len, nonce, aad, sizeof(aad),
                                               tag, buf, buf) == 0;
    ok = ok && memcmp(buf, pt, len) == 0;

    mbedtls_chachapoly_free(&ctx);
    return ok;
}

bool selftest_chachapoly(void)
{
    static unsigned char area[256] __attribute__((aligned(4)));
    size_t len = sizeof(pt) - 1; // 114 bytes
    return run_at(area, len) && run_at(area + 13, len) && run_at(area + 2, len);
}

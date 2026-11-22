/**
 * \file poly1305.c
 *
 * \brief Poly1305 authentication algorithm.
 *
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */
#include "common.h"

#if defined(MBEDTLS_POLY1305_C)

#include "mbedtls/poly1305.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/error.h"

#include <string.h>

#include "mbedtls/platform.h"

#if !defined(MBEDTLS_POLY1305_ALT)

#define POLY1305_BLOCK_SIZE_BYTES (16U)

/*
 * dspico-romm: the block function and the final reduction are OpenSSL's
 * ARMv4 integer assembly (poly1305-armv4.S, CRYPTOGAMS), which keeps the
 * accumulator in registers and uses umlal chains. It works on a context of
 * h[5] at word 0 and the clamped r[4] at word 5; the NEON-only fields are
 * never touched on ARMv5. The mbedtls context (72 bytes) is reused as:
 *   words 0-4   h (accumulator)       (over r[], s[0])
 *   words 5-8   r                     (over s[1..3], acc[0])
 *   words 9-12  s                     (over acc[1..4])
 *   queue[], queue_len                unchanged
 */
void poly1305_blocks(void *ctx, const unsigned char *inp, size_t len,
                     uint32_t padbit);
void poly1305_emit(void *ctx, unsigned char mac[16], const uint32_t nonce[4]);

#define PV(ctx) ((uint32_t *) (void *) (ctx))

static void poly1305_process(mbedtls_poly1305_context *ctx,
                             size_t nblocks,
                             const unsigned char *input,
                             uint32_t needs_padding)
{
    poly1305_blocks(ctx, input, nblocks * POLY1305_BLOCK_SIZE_BYTES,
                    needs_padding);
}

static void poly1305_compute_mac(mbedtls_poly1305_context *ctx,
                                 unsigned char mac[16])
{
    poly1305_emit(ctx, mac, &PV(ctx)[9]);
}

void mbedtls_poly1305_init(mbedtls_poly1305_context *ctx)
{
    mbedtls_platform_zeroize(ctx, sizeof(mbedtls_poly1305_context));
}

void mbedtls_poly1305_free(mbedtls_poly1305_context *ctx)
{
    if (ctx == NULL) {
        return;
    }

    mbedtls_platform_zeroize(ctx, sizeof(mbedtls_poly1305_context));
}

int mbedtls_poly1305_starts(mbedtls_poly1305_context *ctx,
                            const unsigned char key[32])
{
    _Static_assert(sizeof(mbedtls_poly1305_context) >= 13 * 4 + 16 + 4,
                   "context too small for the assembly layout");
    uint32_t *v = PV(ctx);

    /* h = 0 */
    v[0] = v[1] = v[2] = v[3] = v[4] = 0U;

    /* r &= 0x0ffffffc0ffffffc0ffffffc0fffffff */
    v[5] = MBEDTLS_GET_UINT32_LE(key, 0)  & 0x0FFFFFFFU;
    v[6] = MBEDTLS_GET_UINT32_LE(key, 4)  & 0x0FFFFFFCU;
    v[7] = MBEDTLS_GET_UINT32_LE(key, 8)  & 0x0FFFFFFCU;
    v[8] = MBEDTLS_GET_UINT32_LE(key, 12) & 0x0FFFFFFCU;

    v[9]  = MBEDTLS_GET_UINT32_LE(key, 16);
    v[10] = MBEDTLS_GET_UINT32_LE(key, 20);
    v[11] = MBEDTLS_GET_UINT32_LE(key, 24);
    v[12] = MBEDTLS_GET_UINT32_LE(key, 28);

    /* Queue initially empty */
    mbedtls_platform_zeroize(ctx->queue, sizeof(ctx->queue));
    ctx->queue_len = 0U;

    return 0;
}

int mbedtls_poly1305_update(mbedtls_poly1305_context *ctx,
                            const unsigned char *input,
                            size_t ilen)
{
    size_t offset    = 0U;
    size_t remaining = ilen;
    size_t queue_free_len;
    size_t nblocks;

    if ((remaining > 0U) && (ctx->queue_len > 0U)) {
        queue_free_len = (POLY1305_BLOCK_SIZE_BYTES - ctx->queue_len);

        if (ilen < queue_free_len) {
            /* Not enough data to complete the block.
             * Store this data with the other leftovers.
             */
            memcpy(&ctx->queue[ctx->queue_len],
                   input,
                   ilen);

            ctx->queue_len += ilen;

            remaining = 0U;
        } else {
            /* Enough data to produce a complete block */
            memcpy(&ctx->queue[ctx->queue_len],
                   input,
                   queue_free_len);

            ctx->queue_len = 0U;

            poly1305_process(ctx, 1U, ctx->queue, 1U);   /* add padding bit */

            offset    += queue_free_len;
            remaining -= queue_free_len;
        }
    }

    if (remaining >= POLY1305_BLOCK_SIZE_BYTES) {
        nblocks = remaining / POLY1305_BLOCK_SIZE_BYTES;

        poly1305_process(ctx, nblocks, &input[offset], 1U);

        offset += nblocks * POLY1305_BLOCK_SIZE_BYTES;
        remaining %= POLY1305_BLOCK_SIZE_BYTES;
    }

    if (remaining > 0U) {
        /* Store partial block */
        ctx->queue_len = remaining;
        memcpy(ctx->queue, &input[offset], remaining);
    }

    return 0;
}

int mbedtls_poly1305_finish(mbedtls_poly1305_context *ctx,
                            unsigned char mac[16])
{
    /* Process any leftover data */
    if (ctx->queue_len > 0U) {
        /* Add padding bit */
        ctx->queue[ctx->queue_len] = 1U;
        ctx->queue_len++;

        /* Pad with zeroes */
        memset(&ctx->queue[ctx->queue_len],
               0,
               POLY1305_BLOCK_SIZE_BYTES - ctx->queue_len);

        poly1305_process(ctx, 1U,           /* Process 1 block */
                         ctx->queue, 0U);   /* Already padded above */
    }

    poly1305_compute_mac(ctx, mac);

    return 0;
}

int mbedtls_poly1305_mac(const unsigned char key[32],
                         const unsigned char *input,
                         size_t ilen,
                         unsigned char mac[16])
{
    mbedtls_poly1305_context ctx;
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    mbedtls_poly1305_init(&ctx);

    ret = mbedtls_poly1305_starts(&ctx, key);
    if (ret != 0) {
        goto cleanup;
    }

    ret = mbedtls_poly1305_update(&ctx, input, ilen);
    if (ret != 0) {
        goto cleanup;
    }

    ret = mbedtls_poly1305_finish(&ctx, mac);

cleanup:
    mbedtls_poly1305_free(&ctx);
    return ret;
}

#endif /* MBEDTLS_POLY1305_ALT */

#if defined(MBEDTLS_SELF_TEST)

static const unsigned char test_keys[2][32] =
{
    {
        0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33,
        0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
        0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
        0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b
    },
    {
        0x1c, 0x92, 0x40, 0xa5, 0xeb, 0x55, 0xd3, 0x8a,
        0xf3, 0x33, 0x88, 0x86, 0x04, 0xf6, 0xb5, 0xf0,
        0x47, 0x39, 0x17, 0xc1, 0x40, 0x2b, 0x80, 0x09,
        0x9d, 0xca, 0x5c, 0xbc, 0x20, 0x70, 0x75, 0xc0
    }
};

static const unsigned char test_data[2][127] =
{
    {
        0x43, 0x72, 0x79, 0x70, 0x74, 0x6f, 0x67, 0x72,
        0x61, 0x70, 0x68, 0x69, 0x63, 0x20, 0x46, 0x6f,
        0x72, 0x75, 0x6d, 0x20, 0x52, 0x65, 0x73, 0x65,
        0x61, 0x72, 0x63, 0x68, 0x20, 0x47, 0x72, 0x6f,
        0x75, 0x70
    },
    {
        0x27, 0x54, 0x77, 0x61, 0x73, 0x20, 0x62, 0x72,
        0x69, 0x6c, 0x6c, 0x69, 0x67, 0x2c, 0x20, 0x61,
        0x6e, 0x64, 0x20, 0x74, 0x68, 0x65, 0x20, 0x73,
        0x6c, 0x69, 0x74, 0x68, 0x79, 0x20, 0x74, 0x6f,
        0x76, 0x65, 0x73, 0x0a, 0x44, 0x69, 0x64, 0x20,
        0x67, 0x79, 0x72, 0x65, 0x20, 0x61, 0x6e, 0x64,
        0x20, 0x67, 0x69, 0x6d, 0x62, 0x6c, 0x65, 0x20,
        0x69, 0x6e, 0x20, 0x74, 0x68, 0x65, 0x20, 0x77,
        0x61, 0x62, 0x65, 0x3a, 0x0a, 0x41, 0x6c, 0x6c,
        0x20, 0x6d, 0x69, 0x6d, 0x73, 0x79, 0x20, 0x77,
        0x65, 0x72, 0x65, 0x20, 0x74, 0x68, 0x65, 0x20,
        0x62, 0x6f, 0x72, 0x6f, 0x67, 0x6f, 0x76, 0x65,
        0x73, 0x2c, 0x0a, 0x41, 0x6e, 0x64, 0x20, 0x74,
        0x68, 0x65, 0x20, 0x6d, 0x6f, 0x6d, 0x65, 0x20,
        0x72, 0x61, 0x74, 0x68, 0x73, 0x20, 0x6f, 0x75,
        0x74, 0x67, 0x72, 0x61, 0x62, 0x65, 0x2e
    }
};

static const size_t test_data_len[2] =
{
    34U,
    127U
};

static const unsigned char test_mac[2][16] =
{
    {
        0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6,
        0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9
    },
    {
        0x45, 0x41, 0x66, 0x9a, 0x7e, 0xaa, 0xee, 0x61,
        0xe7, 0x08, 0xdc, 0x7c, 0xbc, 0xc5, 0xeb, 0x62
    }
};

/* Make sure no other definition is already present. */
#undef ASSERT

#define ASSERT(cond, args)            \
    do                                  \
    {                                   \
        if (!(cond))                \
        {                               \
            if (verbose != 0)          \
            mbedtls_printf args;    \
                                        \
            return -1;               \
        }                               \
    }                                   \
    while (0)

int mbedtls_poly1305_self_test(int verbose)
{
    unsigned char mac[16];
    unsigned i;
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    for (i = 0U; i < 2U; i++) {
        if (verbose != 0) {
            mbedtls_printf("  Poly1305 test %u ", i);
        }

        ret = mbedtls_poly1305_mac(test_keys[i],
                                   test_data[i],
                                   test_data_len[i],
                                   mac);
        ASSERT(0 == ret, ("error code: %i\n", ret));

        ASSERT(0 == memcmp(mac, test_mac[i], 16U), ("failed (mac)\n"));

        if (verbose != 0) {
            mbedtls_printf("passed\n");
        }
    }

    if (verbose != 0) {
        mbedtls_printf("\n");
    }

    return 0;
}

#endif /* MBEDTLS_SELF_TEST */

#endif /* MBEDTLS_POLY1305_C */

// SPDX-License-Identifier: MIT
#ifndef ROMM_DL_H
#define ROMM_DL_H

// Minimal HTTPS client for large downloads (ROMs), without libcurl.
//
// The body is decrypted by Mbed TLS directly into the destination buffer
// (the SD writer's ring, see sdw_reserve), so every byte is copied once
// after TLS instead of three times (TLS -> curl buffer -> write callback ->
// ring). One keep-alive connection to the configured server is kept open
// between calls. API requests still use libcurl (net.h).

#include <stdbool.h>
#include <stdint.h>

#include "net.h"

struct sdwriter;

// GET bytes [first, last] of <url><path> with the Bearer token. The body goes
// into sdw, or is discarded when sdw is NULL. Fills res like net.h does.
bool dl_get_range(const char *path, uint64_t first, uint64_t last,
                  struct sdwriter *sdw, net_progress_fn cb, void *ctx,
                  net_result_t *res);

// Yield to lwIP's thread after every TLS record (default true; speed test A/B).
extern bool dl_yield_per_record;

// Decrypt TLS records directly from the network buffers (default true).
extern bool dl_zero_copy_tls;

// Closes the keep-alive connection (the next call reconnects).
void dl_close(void);

#endif

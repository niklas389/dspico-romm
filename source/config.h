// SPDX-License-Identifier: MIT
#ifndef ROMM_CONFIG_H
#define ROMM_CONFIG_H

#include <stdbool.h>

#define CFG_URL_MAX   256
#define CFG_TOKEN_MAX 256
#define CFG_PATH_MAX  128

typedef struct
{
    char base_dir[CFG_PATH_MAX]; // e.g. "sd:/romm/" (always ends with '/')
    char url[CFG_URL_MAX];       // e.g. "https://romm.example.com" (no trailing '/')
    char http_url[CFG_URL_MAX];  // Optional plain-HTTP URL, speed test only
    char token[CFG_TOKEN_MAX];   // Bearer token from device pairing, may be empty
    char save_slot[32];          // RomM save slot to sync (default "autosave")
    bool sdraw;                  // DSpico: DMA SD writes for downloads (default on)
    bool zerocopy;               // Zero-copy Wi-Fi RX (default on)
    int mtu;                     // Wi-Fi MTU, sets the TCP MSS (default 1500)
    bool insecure;               // Skip certificate verification (diagnostics only)
    bool verbose;                // Write curl debug traces to the log file
} config_t;

// Finds the "romm/" folder (on the boot drive first, then sd:/ and fat:/) and
// loads config.ini and token.txt from it. Returns false if no config.ini was
// found or the url key is missing.
bool config_load(config_t *cfg);

// Builds "<base_dir><name>" into out.
void config_path(const config_t *cfg, const char *name, char *out, int out_len);

bool config_save_token(const config_t *cfg, const char *token);
void config_delete_token(const config_t *cfg);

#endif

// SPDX-License-Identifier: MIT
#ifndef ROMM_SAVESYNC_H
#define ROMM_SAVESYNC_H

// Save sync with RomM (RomM 5.5 save API with device tracking).
//
// Pico Launcher keeps a game's save next to the ROM: <rom stem>.sav. Per ROM
// the app compares three states:
//   L  MD5 of the local .sav
//   S  the newest server save of the ROM in the configured slot (save_slot),
//      its content_hash, and whether RomM marks it current for this device
//   B  the state at the last sync (local MD5 and server content_hash),
//      stored in <base>/saves/<rom_id>.txt
// Only local changed -> upload; only server changed -> download; both ->
// conflict (the user decides). Before a local save is replaced it is copied
// to <base>/save-backup/. A DeSmuME footer on downloaded saves is removed.

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "config.h"
#include "romindex.h"

enum
{
    SS_NONE,        // No save anywhere
    SS_IN_SYNC,
    SS_UPLOAD,      // Local is newer (or only local exists)
    SS_DOWNLOAD,    // Server is newer (or only the server has one)
    SS_CONFLICT,    // Both changed, or never synced and different
    SS_ERROR,
};

typedef struct
{
    int id;
    int rom_id;
    uint32_t size;
    bool current;           // RomM: this device has the latest version
    char updated_at[32];
    char hash[48];          // content_hash, may be empty
    char file_name[128];
} ss_server_save_t;

typedef struct
{
    int rom_id;
    int action;             // SS_*
    bool have_local;
    uint32_t local_size;
    time_t local_mtime;
    char local_hash[33];
    char local_path[CFG_PATH_MAX + 256];
    bool have_server;
    ss_server_save_t srv;
    char base_local[33];
    char base_server[48];
} ss_status_t;

typedef struct
{
    ss_server_save_t *v;
    int count;
} ss_list_t;

// Finds (and caches in <base>/device.txt) this device's RomM device id.
bool ss_init(const config_t *cfg);
const char *ss_device_id(void);

// Server saves of a platform or of one ROM (pass 0 for the other), filtered
// to the configured slot.
bool ss_fetch(const config_t *cfg, int platform_id, int rom_id, ss_list_t *out);
void ss_list_free(ss_list_t *l);

// Status of one ROM in the library folder (needs the ROM on the SD card).
void ss_status(const config_t *cfg, const rom_entry_t *e, const char *folder,
               const ss_list_t *server, ss_status_t *st);

// Performs SS_UPLOAD or SS_DOWNLOAD for st (also to resolve a conflict).
bool ss_apply(const config_t *cfg, const ss_status_t *st, int action, char *err,
              int err_len);

const char *ss_action_name(int action);

// The decision itself (savesync_rules.c): hashes are lowercase hex, "" when
// unknown; base_* are the states recorded at the last sync.
int ss_decide(bool have_local, bool have_server, const char *local_hash,
              const char *server_hash, bool server_current, const char *base_local,
              const char *base_server);

#endif

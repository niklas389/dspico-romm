// SPDX-License-Identifier: MIT
//
// The save sync decision (see savesync.h), free of I/O so it can be tested on
// the host (tools/hosttest/savesync_host.c).

#include "savesync.h"

#include <string.h>

int ss_decide(bool have_local, bool have_server, const char *local_hash,
              const char *server_hash, bool server_current, const char *base_local,
              const char *base_server)
{
    if (!have_local && !have_server)
        return SS_NONE;
    if (have_local && !have_server)
        return SS_UPLOAD;
    if (!have_local)
        return SS_DOWNLOAD;
    if (server_hash[0] && strcmp(server_hash, local_hash) == 0)
        return SS_IN_SYNC;
    if (!base_local[0])
        return SS_CONFLICT; // Both exist, never synced, different

    bool local_changed = strcmp(local_hash, base_local) != 0;
    // The server side by its own hash when both are known, else by RomM's
    // "current for this device" flag
    bool server_changed = server_hash[0] && base_server[0]
                              ? strcmp(server_hash, base_server) != 0
                              : !server_current;
    if (!local_changed && !server_changed)
        return SS_IN_SYNC;
    if (local_changed && !server_changed)
        return SS_UPLOAD;
    if (!local_changed && server_changed)
        return SS_DOWNLOAD;
    return SS_CONFLICT;
}

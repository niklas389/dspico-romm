// SPDX-License-Identifier: MIT
// Host test of the save sync decision (source/savesync_rules.c):
//   cc -Isource tools/hosttest/savesync_host.c source/savesync_rules.c -o t && ./t
#include <stdio.h>
#include "savesync.h"

static int fails;
static void t(const char *what, int want, bool hl, bool hs, const char *L, const char *S,
              bool cur, const char *BL, const char *BS)
{
    int got = ss_decide(hl, hs, L, S, cur, BL, BS);
    if (got != want)
    {
        printf("FAIL %-45s got %s want %s\n", what, ss_action_name(got), ss_action_name(want));
        fails++;
    }
}

const char *ss_action_name(int a)
{
    static const char *n[] = { "none", "in sync", "upload", "download", "conflict", "error" };
    return n[a];
}

int main(void)
{
    t("nothing", SS_NONE, 0, 0, "", "", 0, "", "");
    t("only local", SS_UPLOAD, 1, 0, "aa", "", 0, "", "");
    t("only server", SS_DOWNLOAD, 0, 1, "", "bb", 0, "", "");
    t("same hash, never synced", SS_IN_SYNC, 1, 1, "aa", "aa", 0, "", "");
    t("different, never synced", SS_CONFLICT, 1, 1, "aa", "bb", 0, "", "");
    t("nothing changed", SS_IN_SYNC, 1, 1, "aa", "xx", 1, "aa", "xx");
    t("local changed", SS_UPLOAD, 1, 1, "a2", "xx", 1, "aa", "xx");
    t("server changed", SS_DOWNLOAD, 1, 1, "aa", "x2", 0, "aa", "xx");
    t("both changed", SS_CONFLICT, 1, 1, "a2", "x2", 0, "aa", "xx");
    // Server without content_hash: RomM's is_current flag decides
    t("no server hash, current, local same", SS_IN_SYNC, 1, 1, "aa", "", 1, "aa", "");
    t("no server hash, current, local changed", SS_UPLOAD, 1, 1, "a2", "", 1, "aa", "");
    t("no server hash, not current", SS_DOWNLOAD, 1, 1, "aa", "", 0, "aa", "");
    t("no server hash, not current, local chg", SS_CONFLICT, 1, 1, "a2", "", 0, "aa", "");
    // Server hash in another scheme than MD5 (never equal to L)
    t("other scheme, unchanged", SS_IN_SYNC, 1, 1, "aa", "sha:1", 1, "aa", "sha:1");
    t("other scheme, server new", SS_DOWNLOAD, 1, 1, "aa", "sha:2", 1, "aa", "sha:1");
    t("local deleted, server exists", SS_DOWNLOAD, 0, 1, "", "xx", 1, "aa", "xx");
    printf("%d failures\n", fails);
    return fails != 0;
}

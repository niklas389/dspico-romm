// SPDX-License-Identifier: MIT
//
// Renders the UI screens with sample data into PPM files (top above bottom),
// to check layouts without hardware. Build and run:
//   cc -O1 -DGFX_HOST -Isource -o /tmp/ui_host tools/hosttest/ui_host.c
//      source/gfx.c source/ui.c source/font_data.c -lm, then /tmp/ui_host outdir

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "ui.h"

static const char *outdir = ".";

static void save(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", GFX_W, GFX_H * 2 + 6);
    for (int s = 0; s < 3; s++)
    {
        if (s == 1)
        {
            for (int i = 0; i < GFX_W * 6; i++)
                fputc(20, f), fputc(20, f), fputc(20, f);
            continue;
        }
        const uint16_t *fb = s == 0 ? gfx_top : gfx_bot;
        for (int i = 0; i < GFX_W * GFX_H; i++)
        {
            uint16_t c = fb[i];
            fputc(((c & 31) * 255) / 31, f);
            fputc((((c >> 5) & 31) * 255) / 31, f);
            fputc((((c >> 10) & 31) * 255) / 31, f);
        }
    }
    fclose(f);
}

static const char *names[] = {
    "Mario Kart DS", "Mario Party DS", "Meteos", "Metroid Prime Hunters",
    "Monster Rancher DS", "Moon", "Mr. Driller: Drill Spirits", "New Super Mario Bros.",
    "Nintendogs", "Phoenix Wright: Ace Attorney", "Pokémon White Version",
};

int main(int argc, char **argv)
{
    if (argc > 1)
        outdir = argv[1];
    gfx_alloc();

    // Game screen
    int n = sizeof(names) / sizeof(names[0]);
    ui_tile_t tiles[16];
    char labels[16][8];
    bool letters[27] = { 0 };
    for (int i = 0; i < n; i++)
    {
        ui_initials(names[i], labels[i], sizeof(labels[i]));
        tiles[i] = (ui_tile_t){ names[i], labels[i], ui_name_color(names[i]), NULL, 0, 0,
                                (uint8_t)(i % 3 == 0 ? UI_TILE_ON_SD : 0) };
        letters[ui_letter_of(names[i])] = true;
    }
    tiles[1].flags |= UI_TILE_WARN;
    ui_game_t g = {
        .name = "Mario Party DS", .platform = "Nintendo DS", .regions = "Europe",
        .genres = "Party, Board game", .players = "1-4",
        .summary = "Bowser shrinks Mario and friends with a magic machine. Play 70+ "
                   "mini-games using the stylus, microphone and buttons.",
        .year = 2007, .size = 64u << 20, .on_sd = true, .save = "conflict",
        .save_kind = UI_WARN, .clock = "19:42",
    };
    ui_draw_game_top(gfx_top, &g);
    ui_strip_t s = { "Nintendo DS", "11 games", tiles, n, 1, 1 * 256,
                     "64.0 MB  \xC2\xB7  on SD card", letters, ui_letter_of(names[1]),
                     "\xE2\x97\x80\xE2\x96\xB6 game  L/R letter  A download  Y save  B back" };
    ui_draw_strip(gfx_bot, &s);
    save("games");

    // Mid-animation
    s.pos_q8 = 1 * 256 + 100;
    ui_draw_strip(gfx_bot, &s);
    save("games_anim");

    // Long title, not on SD
    g.name = "Mr. Driller: Drill Spirits – Extra Long Title Edition";
    g.on_sd = false;
    g.save = NULL;
    g.archive = true;
    ui_draw_game_top(gfx_top, &g);
    s.sel = 6;
    s.pos_q8 = 6 * 256;
    s.letter = ui_letter_of(names[6]);
    ui_draw_strip(gfx_bot, &s);
    save("games_long");

    // Platforms
    static const char *pnames[] = { "Game Boy", "Game Boy Advance", "Game Boy Color",
                                    "Nintendo DS", "Nintendo DSi", "Super Nintendo" };
    static const char *plabels[] = { "GB", "GBA", "GBC", "NDS", "DSi", "SNES" };
    ui_tile_t pt[6];
    for (int i = 0; i < 6; i++)
        pt[i] = (ui_tile_t){ pnames[i], plabels[i], ui_name_color(pnames[i]), NULL, 0, 0,
                             (uint8_t)(i == 5 ? UI_TILE_DIMMED : 0) };
    ui_platform_t p = { "Game Boy Advance", "GBA", "gba", 48, 12, false,
                        "Saves: 2 uploaded, 1 downloaded, 1 conflict", "19:42" };
    ui_draw_platform_top(gfx_top, &p);
    ui_strip_t ps = { "Platforms", "6 shown", pt, 6, 1, 256, "48 games  \xC2\xB7  12 on SD",
                      NULL, 0, "A open  Y show hidden  SELECT menu  START quit" };
    ui_draw_strip(gfx_bot, &ps);
    save("platforms");

    // Start-up
    ui_draw_splash_top(gfx_top, "Version 0.47.0", "romm.example.org");
    ui_step_t st[] = { { "SD card", 2, NULL }, { "Wi-Fi", 2, "192.168.1.144" },
                       { "RomM server", 1, "5.5.0" }, { "Sign in", 0, NULL },
                       { "Save sync", 0, NULL } };
    ui_draw_steps(gfx_bot, "Starting", st, 5, NULL);
    save("startup");

    // Error
    ui_draw_message(gfx_bot, UI_ERROR, "Wi-Fi connect failed",
                    "Set up the access point in DSi System Settings (slot 4-6 for WPA2). "
                    "If it worked before: switch the DSi off and on.\n\nLog: "
                    "fat:/romm/poc-32x1460-rx128.log",
                    "START: exit");
    save("error");

    // Download
    g.name = "Pokémon White Version";
    g.on_sd = false;
    ui_draw_game_top(gfx_top, &g);
    ui_progress_t pr = { "Downloading", "Pokemon - White Version (Europe).zip", 437,
                         "56.1 / 128.4 MB", "1.31 MB/s  \xC2\xB7  0:55 left",
                         { "Download", "Unpack", NULL }, 0, "B: cancel", UI_INFO };
    ui_draw_progress(gfx_bot, &pr);
    save("download");

    // Save sync
    ui_savesync_t ss = { "Animal Crossing: Wild World", "date unknown", "256 KB",
                         "2026-08-14 18:02", "256 KB", "Both changed", UI_WARN,
                         "Keep which version?",
                         "L: keep DSi (upload)  R: keep RomM  B: cancel" };
    ui_draw_savesync(gfx_bot, &ss);
    save("savesync");

    // Hidden menu
    const char *items[] = { "Show log", "Speed test (this game)", "Unzip CPU test", "Back" };
    ui_draw_list(gfx_bot, "Tools", items, 4, 1, "A select  B back");
    save("menu");

    // Pairing
    ui_draw_pairing(gfx_bot, "https://romm.example.org/pair/device", "ABCD-1234", 587);
    save("pairing");
    return 0;
}

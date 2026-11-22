// SPDX-License-Identifier: MIT
#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <fat.h>

static void trim(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static bool find_base_dir(config_t *cfg)
{
    const char *candidates[3] = { fatGetDefaultDrive(), "sd:/", "fat:/" };

    for (int i = 0; i < 3; i++)
    {
        const char *drive = candidates[i];
        if (drive == NULL || drive[0] == '\0')
            continue;

        size_t dlen = strlen(drive);
        const char *sep = (drive[dlen - 1] == '/') ? "" : "/";

        char probe[CFG_PATH_MAX + 16];
        snprintf(cfg->base_dir, sizeof(cfg->base_dir), "%s%sromm/", drive, sep);
        snprintf(probe, sizeof(probe), "%sconfig.ini", cfg->base_dir);

        if (file_exists(probe))
            return true;
    }

    cfg->base_dir[0] = '\0';
    return false;
}

void config_path(const config_t *cfg, const char *name, char *out, int out_len)
{
    snprintf(out, out_len, "%s%s", cfg->base_dir, name);
}

static bool parse_bool(const char *v)
{
    return strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "yes") == 0;
}

bool config_load(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    snprintf(cfg->save_slot, sizeof(cfg->save_slot), "autosave");
    cfg->mtu = 1500;
    cfg->zerocopy = true;
    cfg->a7cmd53 = true;
    cfg->a7dma = true;
    cfg->wifireset = true;
    cfg->sdraw = true;

    if (!find_base_dir(cfg))
        return false;

    char path[CFG_PATH_MAX + 16];
    config_path(cfg, "config.ini", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        trim(line);
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#' || line[0] == '[')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';

        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcasecmp(key, "url") == 0)
        {
            snprintf(cfg->url, sizeof(cfg->url), "%s", val);
            size_t len = strlen(cfg->url);
            while (len > 0 && cfg->url[len - 1] == '/')
                cfg->url[--len] = '\0';
        }
        else if (strcasecmp(key, "http_url") == 0)
        {
            snprintf(cfg->http_url, sizeof(cfg->http_url), "%s", val);
            size_t len = strlen(cfg->http_url);
            while (len > 0 && cfg->http_url[len - 1] == '/')
                cfg->http_url[--len] = '\0';
        }
        else if (strcasecmp(key, "save_slot") == 0)
        {
            if (val[0])
                snprintf(cfg->save_slot, sizeof(cfg->save_slot), "%s", val);
        }
        else if (strcasecmp(key, "sdraw") == 0)
        {
            cfg->sdraw = parse_bool(val);
        }
        else if (strcasecmp(key, "zerocopy") == 0)
        {
            cfg->zerocopy = parse_bool(val);
        }
        else if (strcasecmp(key, "a7cmd53") == 0)
        {
            cfg->a7cmd53 = parse_bool(val);
        }
        else if (strcasecmp(key, "a7dma") == 0)
        {
            cfg->a7dma = parse_bool(val);
        }
        else if (strcasecmp(key, "wifireset") == 0)
        {
            cfg->wifireset = parse_bool(val);
        }
        else if (strcasecmp(key, "mtu") == 0)
        {
            int v = atoi(val);
            if (v >= 576 && v <= 1500)
                cfg->mtu = v;
        }
        else if (strcasecmp(key, "insecure") == 0)
        {
            cfg->insecure = parse_bool(val);
        }
        else if (strcasecmp(key, "verbose") == 0)
        {
            cfg->verbose = parse_bool(val);
        }
    }
    fclose(f);

    // The token is stored separately so config.ini can be shared/edited freely.
    config_path(cfg, "token.txt", path, sizeof(path));
    f = fopen(path, "r");
    if (f)
    {
        if (fgets(cfg->token, sizeof(cfg->token), f))
            trim(cfg->token);
        fclose(f);
    }

    return cfg->url[0] != '\0';
}

bool config_save_token(const config_t *cfg, const char *token)
{
    char path[CFG_PATH_MAX + 16];
    config_path(cfg, "token.txt", path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f)
        return false;

    bool ok = fputs(token, f) >= 0;
    fputc('\n', f);
    ok = (fclose(f) == 0) && ok;
    return ok;
}

void config_delete_token(const config_t *cfg)
{
    char path[CFG_PATH_MAX + 16];
    config_path(cfg, "token.txt", path, sizeof(path));
    unlink(path);
}

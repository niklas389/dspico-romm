// SPDX-License-Identifier: MIT
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static FILE *log_fp;

void log_open(const char *path)
{
    log_fp = fopen(path, "w");
}

void log_close(void)
{
    if (log_fp)
    {
        fclose(log_fp);
        log_fp = NULL;
    }
}

void log_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (log_fp)
    {
        va_start(ap, fmt);
        vfprintf(log_fp, fmt, ap);
        va_end(ap);
        fflush(log_fp);
    }
}

void log_file_only(const char *fmt, ...)
{
    if (!log_fp)
        return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fflush(log_fp);
}

void log_file_write(const char *data, size_t len)
{
    if (!log_fp)
        return;

    fwrite(data, 1, len, log_fp);
    fflush(log_fp);
}

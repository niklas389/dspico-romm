// SPDX-License-Identifier: MIT
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

static FILE *log_fp;
static unsigned log_lines;
static bool log_echo = true;

void log_set_echo(bool on)
{
    log_echo = on;
}

// fflush() only reaches FatFs; the file size in the directory is written by
// fsync()/fclose(). After a crash the log was 0 bytes: sync every 8 lines.
static void log_maybe_sync(void)
{
    if (++log_lines % 8 == 0)
        fsync(fileno(log_fp));
}

void log_sync(void)
{
    if (log_fp)
    {
        fflush(log_fp);
        fsync(fileno(log_fp));
    }
}

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

    if (log_echo)
    {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }

    if (log_fp)
    {
        va_start(ap, fmt);
        vfprintf(log_fp, fmt, ap);
        va_end(ap);
        fflush(log_fp);
        log_maybe_sync();
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

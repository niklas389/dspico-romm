// SPDX-License-Identifier: MIT
#ifndef ROMM_LOG_H
#define ROMM_LOG_H

#include <stdbool.h>
#include <stddef.h>

// Opens the log file on the SD card. Console output works without it.
void log_open(const char *path);
void log_close(void);

// Prints to the bottom-screen console and appends to the log file.
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Appends to the log file only (verbose output such as curl debug traces).
void log_file_only(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_file_write(const char *data, size_t len);

#endif

// SPDX-License-Identifier: MIT
#ifndef ROMM_BENCH_H
#define ROMM_BENCH_H

#include <stdint.h>

#include "config.h"

// Runs the full speed-test suite against one ROM (rom_id / fs_name / size)
// and prints a summary. Results are also written to poc.log.
void bench_run_suite(const config_t *cfg, int rom_id, const char *fs_name,
                     uint64_t rom_size);

#endif

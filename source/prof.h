// SPDX-License-Identifier: MIT
#ifndef ROMM_PROF_H
#define ROMM_PROF_H

#include <stdint.h>

void prof_init(void);
uint64_t prof_ticks(void);
uint32_t prof_ticks_to_ms(uint64_t ticks);

void prof_reset(void);
uint64_t prof_wait_ticks(void); // Main thread blocked in poll/select/recv
void prof_add_sd(uint64_t ticks);
uint64_t prof_sd_ticks(void);   // Time spent in fwrite() for downloads

#endif

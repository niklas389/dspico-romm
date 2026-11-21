// SPDX-License-Identifier: MIT
//
// Lightweight time accounting for the speed test, without adding load.
//
// The socket wait functions curl uses (poll, select, recv) are wrapped with
// the linker (-Wl,--wrap=...). Time spent inside them is time the main thread
// waits for the network; during that time only DSWiFi's lwIP thread runs.

#include "prof.h"

#include <stddef.h>
#include <sys/types.h>

#include <nds.h>

static volatile uint64_t g_wait_ticks;
static volatile uint64_t g_sd_ticks;
static uint32_t g_last;
static uint64_t g_now;

void prof_init(void)
{
    cpuStartTiming(0); // Timers 0+1 cascaded, BUS_CLOCK ticks
    g_last = cpuGetTiming();
    g_now = 0;
}

// 64-bit tick counter; must be called at least every ~128 s (it is, on every
// wrapped call and every progress callback).
uint64_t prof_ticks(void)
{
    uint32_t t = cpuGetTiming();
    g_now += (uint32_t)(t - g_last);
    g_last = t;
    return g_now;
}

uint32_t prof_ticks_to_ms(uint64_t ticks)
{
    return (uint32_t)(ticks * 1000 / BUS_CLOCK);
}

void prof_reset(void)
{
    g_wait_ticks = 0;
    g_sd_ticks = 0;
}

uint64_t prof_wait_ticks(void)
{
    return g_wait_ticks;
}

void prof_add_sd(uint64_t ticks)
{
    g_sd_ticks += ticks;
}

uint64_t prof_sd_ticks(void)
{
    return g_sd_ticks;
}

// Opaque pointer types keep these independent of the libc/lwIP headers; the
// calling convention is identical.
int __real_poll(void *fds, unsigned int nfds, int timeout);
int __real_select(int nfds, void *r, void *w, void *e, void *t);
ssize_t __real_recv(int s, void *buf, size_t len, int flags);

int __wrap_poll(void *fds, unsigned int nfds, int timeout)
{
    uint64_t t0 = prof_ticks();
    int ret = __real_poll(fds, nfds, timeout);
    g_wait_ticks += prof_ticks() - t0;
    return ret;
}

int __wrap_select(int nfds, void *r, void *w, void *e, void *t)
{
    uint64_t t0 = prof_ticks();
    int ret = __real_select(nfds, r, w, e, t);
    g_wait_ticks += prof_ticks() - t0;
    return ret;
}

ssize_t __wrap_recv(int s, void *buf, size_t len, int flags)
{
    uint64_t t0 = prof_ticks();
    ssize_t ret = __real_recv(s, buf, len, flags);
    g_wait_ticks += prof_ticks() - t0;
    return ret;
}

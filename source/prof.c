// SPDX-License-Identifier: MIT
//
// Time accounting and a sampling profiler for the speed test.
//
// Timer 0 runs freely at BUS_CLOCK (33.5 MHz) and overflows every 1.96 ms.
// Its overflow interrupt
//   - extends the counter to 64 bits (prof_ticks), and
//   - records the address the ARM9 was executing when interrupted (sampling
//     profiler, ~511 samples per second). Samples in the cothread sleep
//     function are real idle time (CPU halted waiting for an interrupt).
//
// The socket wait functions curl uses (poll, select, recv) are additionally
// wrapped with the linker (-Wl,--wrap=...) to measure main-thread wait time.

#include "prof.h"

#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#include <nds.h>

// Defined by the BlocksDS ARM9 linker script: top of the IRQ mode stack.
// On IRQ entry the BIOS pushes {r0-r3, r12, lr_irq} there, so the interrupted
// address (+4) is the word just below __sp_irq. libnds disables IME while a
// handler runs, so there is no nesting that could move this frame.
extern char __sp_irq[];

static volatile uint32_t g_hi;        // Timer 0 overflow count
static volatile uint64_t g_wait_ticks;
static volatile uint64_t g_sd_ticks;

static uint32_t *g_samples;
static volatile uint32_t g_nsamples;
static uint32_t g_max_samples;
static volatile bool g_sampling;

void prof_irq_entry(void); // prof_irq.s, calls prof_irq_c(lr_of_interrupted)
void prof_irq_c(uint32_t caller_lr);

void prof_irq_c(uint32_t caller_lr)
{
    g_hi++;

    if (g_sampling && g_nsamples + 2 <= g_max_samples)
    {
        uintptr_t lr_slot = (uintptr_t)__sp_irq - 4; // Saved lr_irq
        uint32_t pc = *(const volatile uint32_t *)lr_slot - 4;
        g_samples[g_nsamples++] = pc;
        g_samples[g_nsamples++] = caller_lr;
    }
}

void prof_init(void)
{
    // ~60 s of samples at 511 Hz
    g_max_samples = 65536; // (pc, lr) pairs
    g_samples = malloc(g_max_samples * sizeof(uint32_t));
    if (!g_samples)
        g_max_samples = 0;

    timerStart(0, ClockDivider_1, 0, prof_irq_entry);
}

uint64_t prof_ticks(void)
{
    int old = enterCriticalSection();
    uint32_t lo = TIMER_DATA(0);
    uint32_t hi = g_hi;
    // An overflow that happened just now may not have been counted yet.
    if ((REG_IF & IRQ_TIMER(0)) && lo < 0x8000)
        hi++;
    leaveCriticalSection(old);
    return ((uint64_t)hi << 16) | lo;
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

void prof_sampling_start(void)
{
    g_nsamples = 0;
    g_sampling = true;
}

uint32_t prof_sampling_stop(void)
{
    g_sampling = false;
    return g_nsamples / 2;
}

bool prof_samples_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    // Format 2: "PRF2", number of (pc, lr) pairs, pairs.
    uint32_t hdr[2] = { 0x32465250, g_nsamples / 2 };
    uint32_t n = g_nsamples;
    bool ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 &&
              fwrite(g_samples, sizeof(uint32_t), n, f) == n;
    return (fclose(f) == 0) && ok;
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

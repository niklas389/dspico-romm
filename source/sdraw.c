// SPDX-License-Identifier: MIT
//
// Direct DSpico SD sector writes (see sdraw.h).

#include "sdraw.h"

#include <string.h>
#include <unistd.h>

#include <nds.h>
#include <nds/arm9/dldi.h>

#include "third_party/fatfs/ff.h"

// libnds builds FatFs with renamed entry points.
FRESULT libnds_f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT libnds_f_sync(FIL *fp);
// libnds DLDI sector cache (source/arm9/libc/fatfs/cache.c)
void cache_sector_invalidate(uint8_t pdrv, uint32_t sector_from, uint32_t sector_to);

// Slot-1 registers (ARM9 owns the slot: the DLDI driver runs on the ARM9)
#define R_AUXSPICNTH (*(vu8 *)0x040001A1)
#define R_ROMCTRL    (*(vu32 *)0x040001A4)
#define R_CMD        ((vu8 *)0x040001A8)
#define R_DATA       (*(vu32 *)0x04100010)

#define ROMCTRL_BUSY   (1u << 31)
#define ROMCTRL_DRQ    (1u << 23)

// Card transfer settings used by pico-loader for the DSpico
#define DSPICO_WRITE_SETTINGS 0xE1486000u // DS -> card, 512 bytes
#define DSPICO_POLL_SETTINGS  0xA7446000u // card -> DS, 4 bytes

#define TIMEOUT 2000000

#define SDRAW_DMA 2

const char *sdraw_dldi_name(void)
{
    return io_dldi_data ? io_dldi_data->friendlyName : "(none)";
}

bool sdraw_is_dspico(void)
{
    const char *n = sdraw_dldi_name();
    return strstr(n, "DSpico") || strstr(n, "DSPico") || strstr(n, "dspico");
}

static FIL *fil_of(FILE *f)
{
    int fd = fileno(f);
    if (fd < 0 || ((((uint32_t)fd) >> 28) & 7) != 1) // FD_TYPE_FAT
        return NULL;
    return (FIL *)(((uint32_t)fd) & 0x0FFFFFFF);
}

int sdraw_map(FILE *f, uint32_t size, sdraw_extent_t *ext, int max_ext)
{
    fflush(f);
    FIL *fp = fil_of(f);
    if (!fp || !fp->obj.fs)
        return -1;
    FATFS *fs = fp->obj.fs;

    // Allocate clusters up to size (no data written), then commit FAT and
    // directory entry.
    if (fp->obj.objsize < size && libnds_f_lseek(fp, size) != FR_OK)
        return -1;
    if (fp->obj.objsize < size || libnds_f_sync(fp) != FR_OK)
        return -1;

    // Fast-seek link map: pairs of (cluster count, first cluster), 0-ended
    static DWORD tbl[2 * 64 + 1];
    tbl[0] = sizeof(tbl) / sizeof(tbl[0]);
    fp->cltbl = tbl;
    FRESULT r = libnds_f_lseek(fp, CREATE_LINKMAP);
    fp->cltbl = NULL;
    if (r != FR_OK)
        return -1;

    int n = 0;
    uint32_t need = (size + 511) / 512, have = 0;
    for (DWORD *t = tbl + 1; t[0] != 0 && have < need; t += 2)
    {
        if (n >= max_ext)
            return -1;
        uint32_t clusters = t[0], clst = t[1];
        if (clst < 2)
            return -1;
        ext[n].lba = fs->database + (clst - 2) * fs->csize;
        ext[n].count = clusters * fs->csize;
        if (have + ext[n].count > need)
            ext[n].count = need - have;
        have += ext[n].count;
        n++;
    }
    libnds_f_lseek(fp, 0);
    return have >= need ? n : -1;
}

static bool wait_clear(uint32_t bit)
{
    for (int i = 0; i < TIMEOUT; i++)
        if (!(R_ROMCTRL & bit))
            return true;
    return false;
}

static bool wait_set(uint32_t bit)
{
    for (int i = 0; i < TIMEOUT; i++)
        if (R_ROMCTRL & bit)
            return true;
    return false;
}

static void set_cmd(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3, uint32_t arg)
{
    R_AUXSPICNTH = 0x80; // ROM mode, no IRQ
    R_CMD[0] = c0;
    R_CMD[1] = c1;
    R_CMD[2] = c2;
    R_CMD[3] = c3;
    R_CMD[4] = arg >> 24;
    R_CMD[5] = arg >> 16;
    R_CMD[6] = arg >> 8;
    R_CMD[7] = arg;
}

// NTR_CMD_ID_GAME_GET_SD_STAT until the card reports the write as done
static bool wait_sd_done(void)
{
    for (int tries = 0; tries < 100000; tries++)
    {
        set_cmd(0xE4, 0, 0, 0, 0);
        R_ROMCTRL = DSPICO_POLL_SETTINGS;
        if (!wait_set(ROMCTRL_DRQ))
            return false;
        uint32_t st = R_DATA;
        if (!wait_clear(ROMCTRL_BUSY))
            return false;
        if (st != 0)
            return true;
    }
    return false;
}

// Sends one sector of a sequential write (docs/commands.md "SD write"):
// NTR_CMD_ID_GAME_WRITE_SD_DATA = F6 E1 0D 9Q SS SS SS SS, Q = 10XY with
// X = first sector, Y = last sector of the sequence.
static bool send_sector(uint32_t lba, const uint32_t *src, bool first, bool last, bool dma)
{
    if (!wait_clear(ROMCTRL_BUSY))
        return false;

    set_cmd(0xF6, 0xE1, 0x0D, 0x98 | (first ? 2 : 0) | (last ? 1 : 0), lba);

    if (dma)
    {
        DMA_CR(SDRAW_DMA) = 0;
        DMA_SRC(SDRAW_DMA) = (uint32_t)src;
        DMA_DEST(SDRAW_DMA) = (uint32_t)&R_DATA;
        DMA_CR(SDRAW_DMA) = DMA_ENABLE | DMA_START_CARD | DMA_32_BIT | DMA_REPEAT |
                            DMA_DST_FIX | DMA_SRC_INC | 1;
        R_ROMCTRL = DSPICO_WRITE_SETTINGS;
        bool ok = wait_clear(ROMCTRL_BUSY);
        DMA_CR(SDRAW_DMA) = 0;
        if (!ok)
        {
            // DMA did not feed the card: complete the transfer with the CPU
            // (zeros) so the card is not left waiting for data, then report.
            for (int i = 0; i < TIMEOUT && (R_ROMCTRL & ROMCTRL_BUSY); i++)
                if (R_ROMCTRL & ROMCTRL_DRQ)
                    R_DATA = 0;
            return false;
        }
        return true;
    }

    R_ROMCTRL = DSPICO_WRITE_SETTINGS;
    for (int i = 0; i < 128; i++)
    {
        if (!wait_set(ROMCTRL_DRQ))
            return false;
        R_DATA = src[i];
    }
    return wait_clear(ROMCTRL_BUSY);
}

bool sdraw_write(uint32_t lba, const void *src, uint32_t count, bool dma)
{
    if (((uintptr_t)src & 3) != 0 || lba == 0)
        return false;
    if (dma)
        DC_FlushRange(src, count * 512);

    // Sequential write: while the DSpico writes sector i-1 to the SD card,
    // sector i is already transferred; then wait for i-1 to complete.
    const uint32_t *s = src;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++)
    {
        ok = send_sector(lba + i, s + i * 128, i == 0, i == count - 1, dma);
        if (ok && i > 0)
            ok = wait_sd_done(); // sector i - 1
    }
    if (ok)
        ok = wait_sd_done(); // last sector
    else
        wait_sd_done();

    cache_sector_invalidate(0, lba, lba + count - 1); // DEV_DLDI
    return ok;
}

// ---------------------------------------------------------------------------
// Interrupt-driven sequential writes (sdraw_async_*)
//
// Sector i goes to the card by DMA; the card's transfer-complete IRQ then
// polls the status of sector i-1 (a 4-byte read, a few microseconds) and
// starts sector i+1. While the DSpico is still busy, timer 1 retries the
// status read 30 us later. The CPU is only needed for those short steps.
// ---------------------------------------------------------------------------

#define SDRAW_TIMER 1

enum { AS_IDLE, AS_SENDING, AS_POLL_PREV, AS_POLL_LAST, AS_DONE, AS_ERROR };

static volatile int as_state = AS_IDLE;
static uint32_t as_lba, as_count, as_i;
static const uint32_t *as_src;
static volatile uint32_t as_irqs, as_polls;
static bool as_ready;

static void as_send(void)
{
    uint32_t i = as_i;
    bool first = i == 0, last = i == as_count - 1;

    R_AUXSPICNTH = 0xC0; // ROM mode + transfer-complete IRQ
    R_CMD[0] = 0xF6;
    R_CMD[1] = 0xE1;
    R_CMD[2] = 0x0D;
    R_CMD[3] = 0x98 | (first ? 2 : 0) | (last ? 1 : 0);
    uint32_t lba = as_lba + i;
    R_CMD[4] = lba >> 24;
    R_CMD[5] = lba >> 16;
    R_CMD[6] = lba >> 8;
    R_CMD[7] = lba;

    DMA_CR(SDRAW_DMA) = 0;
    DMA_SRC(SDRAW_DMA) = (uint32_t)(as_src + i * 128);
    DMA_DEST(SDRAW_DMA) = (uint32_t)&R_DATA;
    DMA_CR(SDRAW_DMA) = DMA_ENABLE | DMA_START_CARD | DMA_32_BIT | DMA_REPEAT |
                        DMA_DST_FIX | DMA_SRC_INC | 1;
    as_state = AS_SENDING;
    R_ROMCTRL = DSPICO_WRITE_SETTINGS;
}

// One status read (IRQs off in the handlers, so this cannot interleave with
// another card command). Returns 1 done, 0 busy, -1 error.
static int as_poll_once(void)
{
    as_polls++;
    R_AUXSPICNTH = 0x80; // no IRQ for the poll itself
    R_CMD[0] = 0xE4;
    for (int k = 1; k < 8; k++)
        R_CMD[k] = 0;
    R_ROMCTRL = DSPICO_POLL_SETTINGS;
    if (!wait_set(ROMCTRL_DRQ))
        return -1;
    uint32_t st = R_DATA;
    if (!wait_clear(ROMCTRL_BUSY))
        return -1;
    return st != 0;
}

static void as_finish(int state)
{
    as_state = state;
    cache_sector_invalidate(0, as_lba, as_lba + as_count - 1);
}

static void as_timer_irq(void);

static void as_step_poll(void)
{
    int r = as_poll_once();
    if (r < 0)
    {
        as_finish(AS_ERROR);
        return;
    }
    if (r == 0)
    {
        // Still writing: try again in ~30 us
        timerStart(SDRAW_TIMER, ClockDivider_1, TIMER_FREQ(33000), as_timer_irq);
        return;
    }
    if (as_state == AS_POLL_LAST)
    {
        as_finish(AS_DONE);
        return;
    }
    // Sector as_i - 1 is done (or as_i was the first): send the next one
    as_i++;
    if (as_i < as_count)
    {
        as_send();
    }
    else
    {
        as_state = AS_POLL_LAST;
        as_step_poll();
    }
}

static void as_timer_irq(void)
{
    timerStop(SDRAW_TIMER);
    as_irqs++;
    if (as_state == AS_POLL_PREV || as_state == AS_POLL_LAST)
        as_step_poll();
}

static void as_card_irq(void)
{
    as_irqs++;
    if (as_state != AS_SENDING)
        return;
    DMA_CR(SDRAW_DMA) = 0;
    if (as_i == 0)
    {
        // First sector: nothing to wait for yet
        as_i++;
        if (as_i < as_count)
            as_send();
        else
        {
            as_state = AS_POLL_LAST;
            as_step_poll();
        }
        return;
    }
    as_state = AS_POLL_PREV; // wait for sector as_i - 1
    as_step_poll();
}

// The DLDI driver must not touch the card while a write sequence runs.
static FN_MEDIUM_WRITESECTORS dldi_write_orig;
static FN_MEDIUM_READSECTORS dldi_read_orig;

static bool dldi_write_guard(sec_t sector, sec_t num, const void *buf)
{
    while (sdraw_async_busy())
        ;
    return dldi_write_orig(sector, num, buf);
}

static bool dldi_read_guard(sec_t sector, sec_t num, void *buf)
{
    while (sdraw_async_busy())
        ;
    return dldi_read_orig(sector, num, buf);
}

bool sdraw_async_init(void)
{
    if (as_ready)
        return true;
    if (!sdraw_is_dspico())
        return false;
    DLDI_INTERFACE *io = (DLDI_INTERFACE *)io_dldi_data;
    dldi_write_orig = io->ioInterface.writeSectors;
    dldi_read_orig = io->ioInterface.readSectors;
    io->ioInterface.writeSectors = dldi_write_guard;
    io->ioInterface.readSectors = dldi_read_guard;

    timerStop(SDRAW_TIMER);
    irqSet(IRQ_TIMER(SDRAW_TIMER), as_timer_irq);
    irqEnable(IRQ_TIMER(SDRAW_TIMER));
    irqSet(IRQ_CARD, as_card_irq);
    irqEnable(IRQ_CARD);
    as_ready = true;
    return true;
}

bool sdraw_async_start(uint32_t lba, const void *src, uint32_t count)
{
    if (!as_ready || sdraw_async_busy() || count == 0 || lba == 0 ||
        ((uintptr_t)src & 3) != 0)
        return false;
    DC_FlushRange(src, count * 512);
    if (!wait_clear(ROMCTRL_BUSY))
        return false;
    int old = enterCriticalSection();
    as_lba = lba;
    as_src = src;
    as_count = count;
    as_i = 0;
    as_send();
    leaveCriticalSection(old);
    return true;
}

bool sdraw_async_busy(void)
{
    int s = as_state;
    return s == AS_SENDING || s == AS_POLL_PREV || s == AS_POLL_LAST;
}

bool sdraw_async_ok(void)
{
    return as_state != AS_ERROR;
}

void sdraw_async_stats(uint32_t *irqs, uint32_t *polls)
{
    *irqs = as_irqs;
    *polls = as_polls;
}

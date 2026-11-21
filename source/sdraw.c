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

static bool write_sector(uint32_t lba, const uint32_t *src, bool dma)
{
    if (!wait_clear(ROMCTRL_BUSY))
        return false;

    // NTR_CMD_ID_GAME_WRITE_SD_DATA, first and last sector of a write
    set_cmd(0xF6, 0xE1, 0x0D, 0x9B, lba);

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
            wait_sd_done();
            return false;
        }
    }
    else
    {
        R_ROMCTRL = DSPICO_WRITE_SETTINGS;
        for (int i = 0; i < 128; i++)
        {
            if (!wait_set(ROMCTRL_DRQ))
                return false;
            R_DATA = src[i];
        }
        if (!wait_clear(ROMCTRL_BUSY))
            return false;
    }
    return wait_sd_done();
}

bool sdraw_write(uint32_t lba, const void *src, uint32_t count, bool dma)
{
    if (((uintptr_t)src & 3) != 0 || lba == 0)
        return false;
    if (dma)
        DC_FlushRange(src, count * 512);

    const uint32_t *s = src;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++)
        ok = write_sector(lba + i, s + i * 128, dma);

    cache_sector_invalidate(0, lba, lba + count - 1); // DEV_DLDI
    return ok;
}

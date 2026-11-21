// SPDX-License-Identifier: MIT
#include "qr.h"

#include <stdint.h>

#include <nds.h>

#include "qrcodegen.h"

#define SCREEN_W 256
#define SCREEN_H 192
#define QR_MAX_VERSION 10

void top_init(void)
{
    videoSetMode(MODE_FB0);
    vramSetBankA(VRAM_A_LCD);
    top_clear();
}

void top_clear(void)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        VRAM_A[i] = RGB15(4, 4, 6) | BIT(15);
}

bool top_show_qr(const char *text)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                              qrcodegen_Mask_AUTO, true))
        return false;

    int size = qrcodegen_getSize(qr);
    const int quiet = 4; // Quiet zone in modules, required by scanners
    int scale = SCREEN_H / (size + 2 * quiet);
    if (scale < 1)
        return false;

    int px = (size + 2 * quiet) * scale;
    int x0 = (SCREEN_W - px) / 2;
    int y0 = (SCREEN_H - px) / 2;

    top_clear();

    const uint16_t white = RGB15(31, 31, 31) | BIT(15);
    const uint16_t black = RGB15(0, 0, 0) | BIT(15);

    for (int y = 0; y < px; y++)
    {
        int my = y / scale - quiet;
        for (int x = 0; x < px; x++)
        {
            int mx = x / scale - quiet;
            bool dark = mx >= 0 && my >= 0 && mx < size && my < size &&
                        qrcodegen_getModule(qr, mx, my);
            VRAM_A[(y0 + y) * SCREEN_W + (x0 + x)] = dark ? black : white;
        }
    }
    return true;
}

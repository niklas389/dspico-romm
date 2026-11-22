// SPDX-License-Identifier: MIT
#include "qr.h"

#include <stdint.h>

#include "gfx.h"

#include "qrcodegen.h"

#define SCREEN_W 256
#define SCREEN_H 192
#define QR_MAX_VERSION 10

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

    const uint16_t white = GFX_RGB(255, 255, 255);
    const uint16_t black = GFX_RGB(0, 0, 0);
    gfx_clear(gfx_top, white);

    for (int y = 0; y < px; y++)
    {
        int my = y / scale - quiet;
        for (int x = 0; x < px; x++)
        {
            int mx = x / scale - quiet;
            bool dark = mx >= 0 && my >= 0 && mx < size && my < size &&
                        qrcodegen_getModule(qr, mx, my);
            gfx_top[(y0 + y) * SCREEN_W + (x0 + x)] = dark ? black : white;
        }
    }
    return true;
}

// SPDX-License-Identifier: MIT
#ifndef ROMM_QR_H
#define ROMM_QR_H

#include <stdbool.h>

// Draws text as a QR code centred on the top screen back buffer (gfx_top).
bool top_show_qr(const char *text);

#endif

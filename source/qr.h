// SPDX-License-Identifier: MIT
#ifndef ROMM_QR_H
#define ROMM_QR_H

#include <stdbool.h>

// The top screen is a 256x192 16-bit framebuffer (VRAM A in LCD mode).
void top_init(void);
void top_clear(void);

// Renders text as a QR code centred on the top screen.
bool top_show_qr(const char *text);

#endif

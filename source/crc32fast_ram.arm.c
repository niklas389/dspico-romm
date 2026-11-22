// SPDX-License-Identifier: MIT
//
// The same CRC-32 with code and tables in main RAM (speed test comparison).

#include "crc32fast.h"

#define CRC_CODE
#define CRC_TABLE
#define CRC_FN crc32_fast_ram

#include "crc32fast_impl.h"

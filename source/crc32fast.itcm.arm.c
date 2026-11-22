// SPDX-License-Identifier: MIT
//
// Slicing-by-4 CRC-32 (see crc32fast.h). Runs from ITCM in ARM mode, tables
// in ITCM.

#include "crc32fast.h"

#ifdef __NDS__
#include <nds/ndstypes.h>
#define CRC_CODE  ITCM_CODE ARM_CODE
#define CRC_TABLE ITCM_BSS
#else
#define CRC_CODE
#define CRC_TABLE
#endif
#define CRC_FN crc32_fast

#include "crc32fast_impl.h"

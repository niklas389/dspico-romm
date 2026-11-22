/* dspico-romm: a second copy of inflate_fast() that stays in main RAM
   (the file name has no ".itcm"), for the speed test. */
#include "zutil.h"
#undef inflate_fast
#define inflate_fast z_inflate_fast_ram
#include "inffast.itcm.arm.c"

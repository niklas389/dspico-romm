// SPDX-License-Identifier: MIT
#ifndef ROMM_UNZIPBENCH_H
#define ROMM_UNZIPBENCH_H

// CPU speed test of the unpacking code (inflate and CRC-32 variants) on the
// first 4 MB of a ROM, compressed in RAM. Results go to the log.
void unzip_bench(const char *rom_path);

#endif

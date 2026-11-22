// SPDX-License-Identifier: MIT
#ifndef ROMM_AESBENCH_H
#define ROMM_AESBENCH_H

// Feasibility test for TLS_AES_128_GCM with the DSi AES engine:
// aes_bench() finds the engine's key/counter/data byte order by comparing
// with Mbed TLS's AES-CTR, then measures and verifies 256 KB through the
// ARM7 (CPU and NDMA feeding). ghash_bench() measures the ARM9 GHASH.
// Results go to the log.

#define BENCH_BYTES_GHASH (256 * 1024)

void aes_bench(void);
void ghash_bench(void);

#endif

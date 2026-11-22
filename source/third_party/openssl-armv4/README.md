# OpenSSL ARMv4 ChaCha20 / Poly1305 (CRYPTOGAMS)

Generated from OpenSSL 3.0.15 `crypto/chacha/asm/chacha-armv4.pl` and
`crypto/poly1305/asm/poly1305-armv4.pl` (Andy Polyakov, CRYPTOGAMS), Apache
License 2.0 (see `LICENSE.txt`):

```bash
perl chacha-armv4.pl linux32 chacha-armv4.itcm.s
perl poly1305-armv4.pl linux32 poly1305-armv4.itcm.s
```

Built for ARMv5TE (`__ARM_ARCH__` 5), so only the integer code paths are
assembled (no NEON, no `OPENSSL_armcap_P`). The `.itcm` in the file names
places the code in ITCM. Used by `../mbedtls-fast/chacha20.itcm.arm.c` and
`../mbedtls-fast/poly1305.itcm.arm.c`; verified against the Mbed TLS C code
under `qemu-arm -cpu arm946` (3000 randomized cases, unaligned buffers,
counter wrap-around).

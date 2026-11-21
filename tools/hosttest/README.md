# Host tests

- `crypto_armv5.c`: OpenSSL ARMv4 ChaCha20/Poly1305 (via `source/third_party/mbedtls-fast`)
  against the original Mbed TLS C code. Needs `ref_chacha20.c` / `ref_poly1305.c`, which are
  the original Mbed TLS 3.6.4 `chacha20.c` / `poly1305.c` with the public functions renamed
  `ref_*`. Build with `arm-linux-gnueabi-gcc -O2 -march=armv5te -marm -static` and run with
  `qemu-arm -cpu arm946`.
- `dl_host.c` + `dl_server.py`: `source/dl.c` against a local TLS 1.3 server
  (`python3 dl_server.py`, self-signed `cert.pem`/`key.pem` for `localhost`), linked
  against a host build of Mbed TLS 3.6.4 with ASan/UBSan.

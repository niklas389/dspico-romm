# zlib 1.3.1 (inflate only)

Unmodified source from zlib 1.3.1 (taken from the `libz-sys` 1.1.20 crate,
`src/zlib/`), license in `LICENSE`. Changes:

- `zconf.h`: `Z_PREFIX`, `NO_GZIP` and `Z_SOLO` defined and `z_errmsg` renamed, so no symbol clashes
  with the toolchain's libz that libcurl links.
- File names: `inffast.itcm.arm.c` (the hot decoding loop runs from ITCM in
  ARM mode), `inflate.arm.c`, `inftrees.arm.c` (ARM mode).

Used by `source/unzip.c` for zip archives.

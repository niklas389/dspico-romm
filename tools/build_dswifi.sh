#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds a patched DSWiFi plus a matching ARM7 core:
#   - TCP receive window of WND_MSS * 1460 bytes (stock: 4 * 1460)
#   - ARM7 -> ARM9 RX ring buffer of RXBUF_KB KiB (stock: 12 KiB)
#
# The RX buffer lives in a struct shared by both CPUs, so the ARM9 library and
# the ARM7 core must always be built together with the same value.
#
# Usage: tools/build_dswifi.sh <wnd_mss> <rxbuf_kb> <output_dir>
# Produces <output_dir>/lib/libdswifi9.a, <output_dir>/include and
# <output_dir>/arm7_dswifi.elf

set -euo pipefail

WND_MSS="${1:?TCP window in MSS units, e.g. 32}"
RXBUF_KB="${2:?RX ring buffer in KiB, e.g. 128}"
OUT="${3:?output directory}"

# Versions used by BlocksDS v1.25.0, matching the installed SDK.
DSWIFI_COMMIT=84d91e8c517bd7d15d38aff039640beaf359c230
SDK_TAG=v1.25.0

if [ "$WND_MSS" -gt 176 ]; then
    echo "TCP_WND too large (max 176 MSS with window scale 2)" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
export BLOCKSDS="${BLOCKSDS:-/opt/wonderful/thirdparty/blocksds/core}"
export BLOCKSDSEXT="${BLOCKSDSEXT:-/opt/wonderful/thirdparty/blocksds/external}"
export PATH="/opt/wonderful/bin:$PATH"

fetch() { # <dir> <url> <ref>
    if [ ! -d "$1/.git" ]; then
        git init -q "$1"
        git -C "$1" remote add origin "$2"
    fi
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q -f FETCH_HEAD
    git -C "$1" clean -qfdx
}

# --- DSWiFi -----------------------------------------------------------------

SRC="$OUT/dswifi-src"
fetch "$SRC" https://codeberg.org/blocksds/dswifi.git "$DSWIFI_COMMIT"

git -C "$SRC" apply "$ROOT/patches/dswifi-tcp-window.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-rx-buffer.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-rx-checksum.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-fast-rx.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-zero-copy-rx.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-arm7-load.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-recv-zc.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-arm7-fast-rx.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-arm7-rx-dma.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-arm7-aes-test.patch"
git -C "$SRC" apply "$ROOT/patches/dswifi-arm7-hwreset.patch"
sed -i "s/#define DSPICO_TCP_WND_MSS 32/#define DSPICO_TCP_WND_MSS $WND_MSS/" \
    "$SRC/source/arm9/lwip/include/lwipopts.h"
sed -i "s/#define DSPICO_RXBUF_KB 12/#define DSPICO_RXBUF_KB $RXBUF_KB/" \
    "$SRC/source/common/wifi_shared.h"
# A7_CMD53=0: ARM7 IRQ path keeps upstream's CMD52 register reads.
if [ "${A7_CMD53:-1}" = "0" ]; then
    sed -i "s/#define DSPICO_A7_CMD53 1/#define DSPICO_A7_CMD53 0/" \
        "$SRC/source/arm7/twl/card.twl.c"
fi
# Windows of 64 KiB and more need TCP window scaling (RFC 7323). The socket
# mailbox must also hold one entry per segment of a full window.
if [ "$WND_MSS" -gt 44 ]; then
    sed -i "s/^#define TCP_WND .*/&\n#define LWIP_WND_SCALE 1\n#define TCP_RCV_SCALE 2/" \
        "$SRC/source/arm9/lwip/include/lwipopts.h"
    sed -i "s/^#define DEFAULT_TCP_RECVMBOX_SIZE .*/#define DEFAULT_TCP_RECVMBOX_SIZE $((WND_MSS * 2))/" \
        "$SRC/source/arm9/lwip/include/lwipopts.h"
fi

make -C "$SRC" -f Makefile.arm9 ENABLE_LWIP=1 -j"$(nproc)" >/dev/null
make -C "$SRC" -f Makefile.arm7 -j"$(nproc)" >/dev/null

rm -rf "$OUT/lib" "$OUT/include"
mkdir -p "$OUT/lib"
cp "$SRC/lib/libdswifi9.a" "$SRC/lib/libdswifi7.a" "$OUT/lib/"
cp -r "$SRC/include" "$OUT/include"

# --- ARM7 core (SDK main_core, linked against the patched libdswifi7) --------

SDK="$OUT/sdk-src"
if [ ! -d "$SDK/.git" ]; then
    git init -q "$SDK"
    git -C "$SDK" remote add origin https://codeberg.org/blocksds/sdk.git
    git -C "$SDK" config core.sparseCheckout true
    echo "sys/arm7/main_core/" > "$SDK/.git/info/sparse-checkout"
fi
git -C "$SDK" fetch -q --depth 1 origin "refs/tags/$SDK_TAG"
git -C "$SDK" checkout -q -f FETCH_HEAD

CORE="$SDK/sys/arm7/main_core"
# Point the core's DSWiFi library directory at the patched build.
sed -i "s|LIBDIRS += \$(BLOCKSDS)/libs/dswifi|LIBDIRS += $OUT|" "$CORE/Makefile.generic"
rm -rf "$CORE/build" "$CORE"/arm7_*.elf
make -C "$CORE" -f Makefile.generic NAME=dswifi USE_DSWIFI=1 >/dev/null
cp "$CORE/arm7_dswifi.elf" "$OUT/arm7_dswifi.elf"

echo "Built DSWiFi: TCP_WND = $((WND_MSS * 1460)) B, RX buffer = $RXBUF_KB KiB"

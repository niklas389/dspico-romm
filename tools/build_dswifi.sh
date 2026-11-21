#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds a DSWiFi ARM9 library (with lwIP) whose TCP receive window is
# WND_MSS * 1460 bytes. Stock DSWiFi uses 4 * 1460 = 5840 bytes, which caps a
# single TCP connection at roughly window / RTT.
#
# Usage: tools/build_dswifi.sh <wnd_mss> <output_dir>
# Produces <output_dir>/lib/libdswifi9.a and <output_dir>/include.

set -euo pipefail

WND_MSS="${1:?window size in MSS units, e.g. 32}"
OUT="${2:?output directory}"

# DSWiFi commit used by BlocksDS v1.25.0 (keeps the ARM7 side compatible).
DSWIFI_COMMIT=84d91e8c517bd7d15d38aff039640beaf359c230

if [ "$WND_MSS" -gt 44 ]; then
    echo "TCP_WND must stay below 64 KiB without window scaling (max 44)" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export BLOCKSDS="${BLOCKSDS:-/opt/wonderful/thirdparty/blocksds/core}"
export BLOCKSDSEXT="${BLOCKSDSEXT:-/opt/wonderful/thirdparty/blocksds/external}"
export PATH="/opt/wonderful/bin:$PATH"

SRC="$OUT/src"
mkdir -p "$OUT"
if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://codeberg.org/blocksds/dswifi.git
fi
git -C "$SRC" fetch -q --depth 1 origin "$DSWIFI_COMMIT"
git -C "$SRC" checkout -q -f FETCH_HEAD
git -C "$SRC" clean -qfdx

git -C "$SRC" apply "$ROOT/patches/dswifi-tcp-window.patch"
sed -i "s/#define DSPICO_TCP_WND_MSS 32/#define DSPICO_TCP_WND_MSS $WND_MSS/" \
    "$SRC/source/arm9/lwip/include/lwipopts.h"

make -C "$SRC" -f Makefile.arm9 ENABLE_LWIP=1 -j"$(nproc)" >/dev/null

mkdir -p "$OUT/lib"
cp "$SRC/lib/libdswifi9.a" "$OUT/lib/"
rm -rf "$OUT/include"
cp -r "$SRC/include" "$OUT/include"
echo "Built DSWiFi with TCP_WND = $WND_MSS * 1460 = $((WND_MSS * 1460)) bytes"

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Boot smoke test in DeSmuME (DS mode, R4 slot-1 with a host folder as SD).
# Catches crashes before/at startup; Wi-Fi/TLS are not exercised.
# Needs: apt-get install desmume xvfb imagemagick
#
# Usage: tools/emu_boot_test.sh <rom.nds> <screenshot.png> [seconds]

set -euo pipefail
ROM="$1"; OUT="$2"; SECS="${3:-20}"
BLOCKSDS="${BLOCKSDS:-/opt/wonderful/thirdparty/blocksds/core}"
WORK="$(mktemp -d)"
mkdir -p "$WORK/sd/romm"
printf 'url=https://192.0.2.1\ninsecure=1\nverbose=0\n' > "$WORK/sd/romm/config.ini"
cp "$ROM" "$WORK/rom.nds"
"$BLOCKSDS/tools/dldipatch/dldipatch" patch "$BLOCKSDS/sys/dldi_r4/r4tf.dldi" "$WORK/rom.nds" >/dev/null
xvfb-run -a -s "-screen 0 600x500x24" sh -c \
  "/usr/games/desmume-cli --disable-sound --slot1=R4 --slot1-fat-dir='$WORK/sd' '$WORK/rom.nds' >/dev/null 2>&1 & sleep $SECS; import -window root '$OUT'; kill %1" >/dev/null 2>&1 || true
rm -rf "$WORK"
echo "Screenshot: $OUT"

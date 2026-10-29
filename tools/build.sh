#!/usr/bin/env bash
# Build (and optionally upload) the clock:  tools/build.sh [upload [PORT]]
set -euo pipefail
if [[ $# -gt 2 || ( $# -gt 0 && "$1" != "upload" ) ]]; then
  echo "Usage: $0 [upload [PORT]]" >&2
  exit 2
fi
cd "$(dirname "$0")/.."
ROOT=$PWD
# ChipVariant=prev3 : ESP32-P4 silicon older than v3.00 (esptool reports "revision v1.x")
FQBN="m5stack:esp32:m5stack_tab5:ChipVariant=prev3,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default"
PORT=${2:-/dev/ttyACM0}
BUILD_UTC=${SOURCE_DATE_EPOCH:-$(date +%s)}
# source file names in the image (assert messages) relative to their tree, not this computer's directories
ADATA=$(arduino-cli config get directories.data 2>/dev/null || echo "$HOME/.arduino15")
AUSER=$(arduino-cli config get directories.user 2>/dev/null || echo "$HOME/Arduino")
FLAGS="-DLV_CONF_INCLUDE_SIMPLE -DCLOCK_BUILD_UTC=$BUILD_UTC -I$ROOT"
FLAGS="$FLAGS -ffile-prefix-map=$ROOT=fancy_clock -ffile-prefix-map=$AUSER=Arduino -ffile-prefix-map=$ADATA=arduino15"
ARGS=(--fqbn "$FQBN" --build-path "$ROOT/build"
      --build-property "compiler.c.extra_flags=$FLAGS"
      --build-property "compiler.cpp.extra_flags=$FLAGS")
arduino-cli compile "${ARGS[@]}" "$ROOT"
if [[ "${1:-}" == "upload" ]]; then
  arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$ROOT/build"
fi

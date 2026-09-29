#!/usr/bin/env bash
# Build (and optionally upload) the clock:  tools/build.sh [upload [PORT]]
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
# ChipVariant=prev3 : ESP32-P4 silicon older than v3.00 (esptool reports "revision v1.x")
FQBN="m5stack:esp32:m5stack_tab5:ChipVariant=prev3,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default"
PORT=${2:-/dev/ttyACM0}
ARGS=(--fqbn "$FQBN" --build-path "$ROOT/build"
      --build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$ROOT"
      --build-property "compiler.cpp.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$ROOT")
arduino-cli compile "${ARGS[@]}" "$ROOT"
if [[ "${1:-}" == "upload" ]]; then
  arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$ROOT/build"
fi

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
# LVGL with the patches in patches/ (lvgl-<version>-*.patch): a patched copy in lvgl-patched/, used instead of the
# installed library; made again when the library or a patch changes
LVGL_SRC=$AUSER/libraries/lvgl
LVGL_VER=$(sed -n 's/^version=//p' "$LVGL_SRC/library.properties")
LVGL=$ROOT/lvgl-patched
PATCHES=("$ROOT"/patches/lvgl-"$LVGL_VER"-*.patch)
[[ -e "${PATCHES[0]}" ]] || { echo "No patches for LVGL $LVGL_VER in patches/ (made for another version?)" >&2; exit 1; }
STAMP="$LVGL_VER $(cat "${PATCHES[@]}" | sha256sum | cut -c1-16) $(stat -c %Y "$LVGL_SRC/library.properties")"
if [[ "$(cat "$LVGL/.patched" 2>/dev/null)" != "$STAMP" ]]; then
  rm -rf "$LVGL"
  cp -a "$LVGL_SRC" "$LVGL"
  for p in "${PATCHES[@]}"; do patch -s -d "$LVGL" -p1 < "$p"; done
  echo "$STAMP" > "$LVGL/.patched"
fi
FLAGS="-DLV_CONF_INCLUDE_SIMPLE -DCLOCK_BUILD_UTC=$BUILD_UTC -I$ROOT"
FLAGS="$FLAGS -ffile-prefix-map=$LVGL=Arduino/libraries/lvgl -ffile-prefix-map=$ROOT=fancy_clock -ffile-prefix-map=$AUSER=Arduino -ffile-prefix-map=$ADATA=arduino15"
# -O2 instead of the core's -Os: LVGL's software renderer is ~15% faster (helpers get inlined); flash has room
ARGS=(--fqbn "$FQBN" --build-path "$ROOT/build" --library "$LVGL"
      --build-property "compiler.optimization_flags=-O2"
      --build-property "compiler.c.extra_flags=$FLAGS"
      --build-property "compiler.cpp.extra_flags=$FLAGS")
arduino-cli compile "${ARGS[@]}" "$ROOT"
if [[ "${1:-}" == "upload" ]]; then
  arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$ROOT/build"
fi

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
for source in tests/test_clock.cpp tests/test_timekeeping.cpp; do
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-unused-function -I. "$source" -o "$TEST_DIR/test_clock"
ASAN_OPTIONS=detect_leaks=0 "$TEST_DIR/test_clock"
done
python3 -m unittest discover -s tests -p 'test_*.py' -v
bash -n tools/build.sh tools/make_fonts.sh

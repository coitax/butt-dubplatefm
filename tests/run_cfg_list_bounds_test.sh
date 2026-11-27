#!/bin/sh
# Build and run the configuration-list bounds tests.
# The suite is expected to fail until the loader stops writing past the
# server and stream-info arrays.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=${TMPDIR:-/tmp}/cfg_list_bounds_test
INC=${TMPDIR:-/tmp}/cfg-list-bounds-inc
mkdir -p "$INC"
# configure.ac turns stream-config visibility on unless --disable-stream-conf.
# util.cpp includes the generated config.h for that macro.
cat > "$INC/config.h" << 'EOF'
#ifndef TEST_SUPPORT_CONFIG_H
#define TEST_SUPPORT_CONFIG_H
#define VISIBILITY 1
#endif
EOF
CXXFLAGS=$(fltk-config --cxxflags)
g++ -std=c++17 -fsanitize=address -fno-omit-frame-pointer -g \
    -ffunction-sections -fdata-sections \
    -I "$INC" -I "$ROOT/src" -I "$ROOT/src/FLTK" $CXXFLAGS \
    -o "$OUT" \
    "$ROOT/tests/cfg_list_bounds_test.cpp" \
    "$ROOT/src/cfg.cpp" \
    "$ROOT/src/parseconfig.cpp" \
    "$ROOT/src/util.cpp" \
    -Wl,--gc-sections -lm
"$OUT"

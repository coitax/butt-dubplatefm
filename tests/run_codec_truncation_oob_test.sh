#!/bin/bash
# Build and run the codec-loader over-read tests.
# They fail while cfg_set_values shrinks codec strings to five bytes
# without a terminator, and pass once those strings are stored with a
# terminator. Known names must round-trip. Any other value must round-trip
# intact or be replaced by the missing-codec default "mp3".
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

cat > "$BUILD/config.h" << 'EOF'
#ifndef CONFIG_H
#define CONFIG_H
#define PACKAGE_VERSION "0.2.17"
#define VISIBILITY 0
#endif
EOF

g++ -std=c++14 -fsanitize=address -fno-omit-frame-pointer -g -O1 \
    -I"$BUILD" -I"$ROOT/src" -I"$ROOT/src/FLTK" \
    $(fltk-config --cxxflags) \
    "$ROOT/tests/codec_truncation_oob_test.cpp" \
    "$ROOT/src/cfg.cpp" \
    "$ROOT/src/parseconfig.cpp" \
    "$ROOT/src/util.cpp" \
    -o "$BUILD/codec_truncation_oob_test" \
    $(fltk-config --ldflags) -lm

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}:halt_on_error=1:abort_on_error=1"

BIN="$BUILD/codec_truncation_oob_test"
fail=0
run() {
    echo "RUN $1 / $2"
    if "$BIN" "$1" "$2"; then
        echo "passed: $1 / $2"
    else
        echo "FAILED: $1 / $2"
        fail=1
    fi
}

run mp3 flac
run ogg wav
run opus flac
run aac aac
run vorbis mp3
run mp3 pcm_s16
run opusEXTRA flacEXTRA
run abcde abcde
long_q="$(python3 -c 'print("Q"*64)')"
long_r="$(python3 -c 'print("R"*64)')"
run "$long_q" mp3
run mp3 "$long_r"

exit "$fail"

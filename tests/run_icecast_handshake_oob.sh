#!/bin/sh
# Compile and run the Icecast handshake tests against src/icecast.cpp.
# AddressSanitizer is required: a one-byte stack write does not always raise
# SIGSEGV, and the test expects that write to abort the process until the
# handshake rejects an over-long status line.
set -eu
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/icecast_handshake_oob_test"
g++ -std=gnu++14 -O1 -g -fno-omit-frame-pointer -fsanitize=address \
    -fno-sanitize-recover=address \
    -I tests/stubs -I src -I src/FLTK -pthread \
    -o "$out" \
    tests/icecast_handshake_oob_test.cpp \
    src/icecast.cpp \
    src/sockfuncs.cpp \
    src/util.cpp \
    src/timer.cpp \
    src/strfuncs.cpp
ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:detect_leaks=0:detect_stack_use_after_return=0}" \
    "$out" "$@"

#!/bin/sh
# Compile and run the configuration file mode tests.
# The tests are expected to fail until the config writers create files as
# mode 0600 and tighten an existing file before writing the source password.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/butt-cfg-file-mode-test
inc=$(mktemp -d "${TMPDIR:-/tmp}/butt-cfg-inc-XXXXXX")
trap 'rm -rf "$inc"' EXIT
# cfg.cpp includes config.h. configure's generated header is gitignored, and
# the writers under test do not depend on its macros.
cat > "$inc/config.h" << 'EOF'
#ifndef config_h
#define config_h
#define PACKAGE_STRING "butt"
#endif
EOF
cflags=$(fltk-config --cxxflags)
ldflags=$(fltk-config --ldflags --use-images)
g++ -std=c++11 -ffunction-sections -fdata-sections \
    -I"$inc" -I"$root/src" -I"$root/src/FLTK" \
    $cflags \
    "$root/tests/cfg_file_mode_test.cpp" "$root/src/cfg.cpp" \
    -Wl,--gc-sections \
    $ldflags \
    -o "$out"
"$out"

#!/bin/sh
# Build and run the source-login credential tests.
#
# From the repository root:
#   sh tests/run_cleartext_login_test.sh
#
# The login tests are expected to fail until Icecast and Shoutcast source
# logins stop sending the source password to a peer that has not presented
# a verified certificate.

set -e
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
SRC="$ROOT/src"
BUILD="$ROOT/tests/build"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++11 -Wall -Wextra -Wno-missing-field-initializers \
-Wno-unused-parameter -Wno-sign-compare \
-I$BUILD -I$SRC -I$SRC/FLTK \
-ffunction-sections -fdata-sections -pthread"
LDFLAGS="-pthread -Wl,--gc-sections -lssl -lcrypto -lm"

mkdir -p "$BUILD"
cat > "$BUILD/config.h" << 'EOF'
#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H
#define PACKAGE "butt"
#define PACKAGE_NAME "butt"
#define PACKAGE_VERSION "0.2.17"
#define PACKAGE_STRING "butt 0.2.17"
#define VERSION "0.2.17"
#define VISIBILITY 1
#define STDC_HEADERS 1
#endif
EOF

objs=""
for src in icecast.cpp shoutcast.cpp sockfuncs.cpp util.cpp strfuncs.cpp timer.cpp; do
    obj="$BUILD/${src%.cpp}.o"
    "$CXX" $CXXFLAGS -c -o "$obj" "$SRC/$src"
    objs="$objs $obj"
done

"$CXX" $CXXFLAGS -c -o "$BUILD/cleartext_login_test.o" "$ROOT/tests/cleartext_login_test.cpp"
"$CXX" -o "$BUILD/cleartext_login_test" $objs "$BUILD/cleartext_login_test.o" $LDFLAGS
set +e
"$BUILD/cleartext_login_test"
status=$?
set -e
exit "$status"

#!/bin/bash
# Build the Linux now-playing helpers and run them against an MPRIS player
# on a private session bus.
#
#   tests/run_now_playing_tests.sh
#
# The helpers are compiled from src/currentTrackLinux.cpp. M_PERTURB fills
# malloc blocks with a non-zero byte, and _FORTIFY_SOURCE checks strcat
# against that block. Exit status is 0 only when every case returns the
# string documented below.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/tests/build"
mkdir -p "$BUILD"

DBUS_CFLAGS="$(pkg-config --cflags dbus-1)"
DBUS_LIBS="$(pkg-config --libs dbus-1)"

g++ -O1 -D_FORTIFY_SOURCE=3 -g -I"$ROOT/src" $DBUS_CFLAGS \
    -o "$BUILD/now_playing_test" \
    "$ROOT/tests/now_playing_test.cpp" \
    "$ROOT/src/currentTrackLinux.cpp" \
    $DBUS_LIBS
gcc -O1 -g $DBUS_CFLAGS \
    -o "$BUILD/mpris_stub" \
    "$ROOT/tests/mpris_stub.c" \
    $DBUS_LIBS

TMP="$(mktemp -d)"
trap 'kill "$BUS_PID" "$PLAYER_PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT
BUS_PID=""
PLAYER_PID=""

dbus-daemon --session --fork --print-address=3 --print-pid=4 \
    3>"$TMP/addr" 4>"$TMP/pid"
BUS_PID="$(cat "$TMP/pid")"
export DBUS_SESSION_BUS_ADDRESS="$(cat "$TMP/addr")"
export MALLOC_PERTURB_=85

fail=0

# id, bus name, playback status, title-or-dash, expected, artists...
run_case() {
    local id="$1"
    local name="$2"
    local status="$3"
    local title="$4"
    local expected="$5"
    shift 5

    "$BUILD/mpris_stub" "$name" "$status" "$title" "$@" \
        >"$TMP/player.out" 2>"$TMP/player.err" &
    PLAYER_PID=$!

    local i
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if grep -q READY "$TMP/player.out" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$PLAYER_PID" 2>/dev/null; then
            break
        fi
        sleep 0.05
    done

    set +e
    "$BUILD/now_playing_test" "$id" "$expected" >"$TMP/test.out" 2>"$TMP/test.err"
    local rc=$?
    set -e
    kill "$PLAYER_PID" 2>/dev/null || true
    wait "$PLAYER_PID" 2>/dev/null || true
    PLAYER_PID=""

    local label="id=$id status=$status title=$title artists=$* expect=$expected"
    if [[ "$rc" -eq 0 ]]; then
        echo "PASS $label"
    else
        echo "FAIL rc=$rc $label"
        echo "--- test stderr ---"
        cat "$TMP/test.err" || true
        echo "--- stub stderr ---"
        cat "$TMP/player.err" || true
        fail=1
    fi
}

RHY="org.gnome.Rhythmbox3"
CLE="org.mpris.MediaPlayer2.clementine"
SPO="org.mpris.MediaPlayer2.spotify"
CAN="org.mpris.MediaPlayer2.cantata"

# Both fields present: "artist - title".
run_case 0 "$RHY" Playing "Feeling Good" "Nina Simone - Feeling Good" "Nina Simone"
run_case 2 "$CLE" Playing "Feeling Good" "Nina Simone - Feeling Good" "Nina Simone"
run_case 3 "$SPO" Playing "Feeling Good" "Nina Simone - Feeling Good" "Nina Simone"
run_case 4 "$CAN" Playing "Feeling Good" "Nina Simone - Feeling Good" "Nina Simone"

# Several artists are joined with ", " before the title separator.
run_case 0 "$RHY" Playing "Feeling Good" "Nina Simone, Someone - Feeling Good" "Nina Simone" "Someone"
run_case 2 "$CLE" Playing "Feeling Good" "Nina Simone, Someone - Feeling Good" "Nina Simone" "Someone"
run_case 3 "$SPO" Playing "Feeling Good" "Nina Simone, Someone - Feeling Good" "Nina Simone" "Someone"
run_case 4 "$CAN" Playing "Feeling Good" "Nina Simone, Someone - Feeling Good" "Nina Simone" "Someone"

# Paused is not Stopped, so the same line is built.
run_case 0 "$RHY" Paused "Feeling Good" "Nina Simone - Feeling Good" "Nina Simone"

# Missing xesam:artist, missing xesam:title, or both: no line, no crash.
run_case 0 "$RHY" Playing "Feeling Good" NULL
run_case 2 "$CLE" Playing "Feeling Good" NULL
run_case 3 "$SPO" Playing "Feeling Good" NULL
run_case 4 "$CAN" Playing "Feeling Good" NULL

run_case 0 "$RHY" Playing - NULL "Nina Simone"
run_case 2 "$CLE" Playing - NULL "Nina Simone"
run_case 3 "$SPO" Playing - NULL "Nina Simone"
run_case 4 "$CAN" Playing - NULL "Nina Simone"

run_case 0 "$RHY" Playing - NULL
run_case 3 "$SPO" Paused - NULL

# Stopped does not read metadata.
run_case 0 "$RHY" Stopped "Feeling Good" NULL "Nina Simone"
run_case 4 "$CAN" Stopped "Feeling Good" NULL "Nina Simone"

if [[ "$fail" -ne 0 ]]; then
    echo "now-playing tests failed"
    exit 1
fi
echo "now-playing tests passed"
exit 0

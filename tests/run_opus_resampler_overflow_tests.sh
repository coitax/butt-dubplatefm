#!/bin/sh
# Build and run the Opus resampler overflow tests.
# They are expected to abort under AddressSanitizer until the resampler
# buffers are sized from the device frame count.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/opus_resampler_overflow_test

# Capture has to be visible to PortAudio. PulseAudio's default source is enough.
if ! pulseaudio --check 2>/dev/null; then
    pulseaudio --start --exit-idle-time=-1
fi

fltk_libs=$(fltk-config --ldflags)
cflags=$(fltk-config --cxxflags)

g++ -std=c++14 -fno-omit-frame-pointer -g -O1 \
    -I "$root/xcode/include" -I "$root/src" -I "$root/src/FLTK" \
    $cflags \
    "$root/tests/opus_resampler_overflow_test.cpp" \
    "$root/src/port_audio.cpp" \
    "$root/src/ringbuffer.cpp" \
    "$root/src/dsp.cpp" \
    "$root/src/Biquad.cpp" \
    "$root/src/cfg.cpp" \
    "$root/src/parseconfig.cpp" \
    "$root/src/util.cpp" \
    "$root/src/strfuncs.cpp" \
    -Wl,--export-dynamic,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=src_short_to_float_array \
    -lsamplerate -lportaudio -lpthread -lmp3lame -lopus -lvorbis -lvorbisenc -logg -lFLAC -lfdk-aac \
    $fltk_libs \
    -o "$out"

for mode in startup-stream startup-record reinit-stream reinit-record large-stream large-record; do
    echo "=== $mode ==="
    "$out" "$mode"
done

echo "all opus resampler overflow tests passed"

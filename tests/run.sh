#!/bin/sh
# run.sh — build and run paige's PTY-based interactive tests.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
CC=${CC:-cc}

make >/dev/null || { echo "build failed"; exit 1; }
mkdir -p tests/build

# forkpty lives in libutil on Linux/FreeBSD (and is harmlessly present on macOS).
if $CC -std=c11 -O2 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
    tests/pty_test.c -lutil -o tests/build/pty_test 2>/dev/null; then
    :
elif $CC -std=c11 -O2 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
    tests/pty_test.c -o tests/build/pty_test; then
    : # some libcs (musl, macOS) need no -lutil
else
    echo "could not build pty_test"; exit 1
fi

tests/build/pty_test

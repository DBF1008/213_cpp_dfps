#!/bin/sh
# Build and run dfps host-side unit tests.
#
# These exercise pure, platform-independent decision logic (no Android headers),
# so they compile and run on the build host with a plain C++17 compiler -- no
# device or NDK toolchain required.
#
#   sh test/run_tests.sh            # uses $CXX, defaulting to c++
#   CXX=g++-13 sh test/run_tests.sh # override the compiler
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-c++}
BIN="$DIR/topapp_switch_detector_test"

echo "[run_tests] compiling with $CXX"
"$CXX" -std=c++17 -Wall -Wextra -O2 -o "$BIN" "$DIR/topapp_switch_detector_test.cpp"

echo "[run_tests] running"
"$BIN"

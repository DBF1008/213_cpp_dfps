#!/usr/bin/env bash
#
# Copyright (C) 2021-2022 Matt Yang
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# Host regression test for the dfps config validator (source/modules/config_parser.cpp).
# Builds a small assert-based harness under AddressSanitizer and runs it. Requires only a
# host C++17 compiler -- no Android NDK, no spdlog, no device. Override the compiler via CXX.

set -e

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$BASEDIR/build_test"
BIN="$OUT/test_config_parser"

mkdir -p "$OUT"

echo ">>> Compiling config_parser regression test (host, C++17, ASAN)"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -fsanitize=address -fno-omit-frame-pointer -g \
    -I"$BASEDIR/source" \
    "$BASEDIR/tests/test_config_parser.cpp" \
    "$BASEDIR/source/modules/config_parser.cpp" \
    -o "$BIN"

echo ">>> Running $BIN"
"$BIN"

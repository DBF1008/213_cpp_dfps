#!/bin/sh
#
# Copyright (C) 2021-2022 Matt Yang
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Build and run the host-side regression tests. These link the real source/utils/misc.cpp
# and exercise ExecCmdSync() directly, so no Android NDK, device, or cmake is required --
# any C++17 compiler on Linux or macOS works.
#
# Usage:   sh test/run_tests.sh        (or: CXX=g++ sh test/run_tests.sh)

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$SCRIPT_DIR")

# Pick a compiler: honour $CXX, otherwise probe common names.
if [ "${CXX:-}" = "" ]; then
    for c in c++ clang++ g++; do
        if command -v "$c" >/dev/null 2>&1; then
            CXX="$c"
            break
        fi
    done
fi
if [ "${CXX:-}" = "" ]; then
    echo "error: no C++ compiler found (set \$CXX)" >&2
    exit 1
fi

OUT_DIR="$ROOT/build/test"
mkdir -p "$OUT_DIR"
BIN="$OUT_DIR/exec_cmd_sync_test"

echo ">>> Building regression tests with $CXX"
"$CXX" -std=c++17 -Wall -I "$ROOT/source" -o "$BIN" \
    "$ROOT/test/exec_cmd_sync_test.cpp" \
    "$ROOT/source/utils/misc.cpp"

echo ">>> Running regression tests"
"$BIN"

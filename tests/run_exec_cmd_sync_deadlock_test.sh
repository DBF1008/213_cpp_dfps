#!/usr/bin/env bash
# Build and run the ExecCmdSync deadlock regression test against the current
# source/utils/misc.cpp. Uses the host compiler; no Android NDK required.
#
# Usage:
#   ./tests/run_exec_cmd_sync_deadlock_test.sh
#
# Exit codes:
#   0  all assertions passed
#   1  one or more assertions failed
#  42  test harness hung (alarm watchdog) -- deadlock regressed

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
BIN="$BUILD_DIR/exec_cmd_sync_deadlock_test"

mkdir -p "$BUILD_DIR"

: "${CXX:=clang++}"

"$CXX" -std=c++17 -O0 -g -Wall -Wextra \
    -I"$ROOT/tests/stubs" -I"$ROOT/source" \
    "$ROOT/tests/exec_cmd_sync_deadlock_test.cpp" \
    "$ROOT/source/utils/misc.cpp" \
    -pthread \
    -o "$BIN"

exec "$BIN"

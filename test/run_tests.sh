#!/bin/bash
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
# Host-side regression tests for the refresh-rate switch/verify/fallback logic.
# These build only the pure (Android-free) units, so they run on a normal
# desktop with just a C++17 compiler -- no Android SDK/NDK or cmake required.

set -e

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BASEDIR}/build/test"
CXX="${CXX:-clang++}"
BIN="${BUILD_DIR}/dfps_tests"

SRCS=(
    "${BASEDIR}/test/test_main.cpp"
    "${BASEDIR}/test/refresh_rate_probe_test.cpp"
    "${BASEDIR}/test/refresh_rate_switcher_test.cpp"
    "${BASEDIR}/source/utils/refresh_rate_probe.cpp"
    "${BASEDIR}/source/modules/refresh_rate_switcher.cpp"
)

# -Wno-deprecated-* silences warnings from spdlog's bundled fmt against a newer
# host libc++; they do not occur in the Android (NDK) build and never come from
# dfps code.
CXXFLAGS=(
    -std=c++17 -fno-rtti -Wall -Wextra
    -Wno-deprecated-declarations -Wno-deprecated-literal-operator
    -I "${BASEDIR}/source" -I "${BASEDIR}/test" -I "${BASEDIR}/thirdparty/spdlog"
)

mkdir -p "${BUILD_DIR}"
echo ">>> compiling host tests with ${CXX}"
"${CXX}" "${CXXFLAGS[@]}" "${SRCS[@]}" -o "${BIN}"

echo ">>> running ${BIN}"
"${BIN}"

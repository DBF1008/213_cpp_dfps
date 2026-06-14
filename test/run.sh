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

# Host regression test for the pure OffscreenPolicy decision core. Needs only a C++17 host
# compiler -- no Android NDK / cmake. Override the compiler with CXX=g++ ./test/run.sh
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
CXX="${CXX:-clang++}"
OUT="$(mktemp -d)/offscreen_policy_test"

"$CXX" -std=c++17 -Wall -Werror -fno-rtti -I "$ROOT/source" \
    "$HERE/offscreen_policy_test.cpp" "$ROOT/source/modules/offscreen_policy.cpp" -o "$OUT"
"$OUT"

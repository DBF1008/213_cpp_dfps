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
# Standalone host test build (not part of the device/NDK build).
#
# These tests cover only the Android-free refresh-rate logic, so they build with
# a normal host compiler. Use the run_tests.sh script for a no-cmake run, or:
#
#     cmake -S test -B build/hosttest && cmake --build build/hosttest && ./build/hosttest/dfps_tests
#
# The simplest gate is: bash test/run_tests.sh

cmake_minimum_required(VERSION 3.13)
project(dfps_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_executable(dfps_tests
    "${CMAKE_CURRENT_LIST_DIR}/test_main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/refresh_rate_probe_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/refresh_rate_switcher_test.cpp"
    "${ROOT}/source/utils/refresh_rate_probe.cpp"
    "${ROOT}/source/modules/refresh_rate_switcher.cpp"
)

target_include_directories(dfps_tests PRIVATE
    "${ROOT}/source"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${ROOT}/thirdparty/spdlog"
)

target_compile_options(dfps_tests PRIVATE
    -Wall -Wextra -fno-rtti
    -Wno-deprecated-declarations -Wno-deprecated-literal-operator
)

enable_testing()
add_test(NAME dfps_tests COMMAND dfps_tests)

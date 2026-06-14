cmake_minimum_required(VERSION 3.13)
project(dfps_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only compile the pure decision logic — no Android/platform dependencies
add_executable(dfps_tests
    test_main.cpp
    test_offscreen_fusion.cpp
    test_cgroup_calibrator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../source/modules/offscreen_state_fusion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../source/modules/cgroup_calibrator.cpp
)

target_include_directories(dfps_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../source
    ${CMAKE_CURRENT_SOURCE_DIR}/../source/modules
)

target_compile_options(dfps_tests PRIVATE -Wall -Werror -g)

enable_testing()
add_test(NAME dfps_tests COMMAND dfps_tests)

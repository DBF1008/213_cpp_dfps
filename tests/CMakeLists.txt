cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only compile the test file, dynamic_fps module, time_counter, and stubs
# (NOT the real platform/misc_android modules — stubs replace them)
set(TEST_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/test_dynamic_fps_fallback.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/stubs_platform.cpp"
    "${CMAKE_SOURCE_DIR}/source/modules/dynamic_fps.cpp"
    "${CMAKE_SOURCE_DIR}/source/platform/module_base.cpp"
    "${CMAKE_SOURCE_DIR}/source/utils/time_counter.cpp"
)

set(TEST_INCS
    "${CMAKE_SOURCE_DIR}/source"
    "${CMAKE_SOURCE_DIR}/thirdparty/spdlog"
)

add_executable(dfps_tests ${TEST_SRCS})
target_include_directories(dfps_tests PRIVATE ${TEST_INCS})
target_link_libraries(dfps_tests PRIVATE spdlog)

set(TEST_COMPILE_FLAGS
    -Wall -D_GNU_SOURCE
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
    -Wno-deprecated -Wno-reorder-ctor -Wno-unused-private-field
)

# macOS doesn't expose timer_t in <time.h>; provide a stub typedef
if(APPLE)
    list(APPEND TEST_COMPILE_FLAGS "-Dtimer_t=unsigned long")
endif()

target_compile_options(dfps_tests PRIVATE ${TEST_COMPILE_FLAGS})
target_link_options(dfps_tests PRIVATE -pthread)

add_test(NAME dfps_fallback COMMAND dfps_tests)

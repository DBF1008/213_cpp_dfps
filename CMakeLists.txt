cmake_minimum_required(VERSION 3.4.1)

project(dfps)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runnable)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runnable)

add_subdirectory(thirdparty)
add_subdirectory(source)

option(BUILD_TESTS "Build regression tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

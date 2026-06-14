cmake_minimum_required(VERSION 3.13)

project(dfps_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runnable)

# spdlog is header-only; reuse the vendored copy
add_library(spdlog INTERFACE)
target_include_directories(spdlog INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/spdlog)

add_executable(config_parser_test
    test_config_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../source/config_parser.cpp
)

target_include_directories(config_parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../source
)

target_link_libraries(config_parser_test PRIVATE spdlog)

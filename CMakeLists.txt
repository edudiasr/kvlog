cmake_minimum_required(VERSION 3.16)
project(kvlog LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(kvlog_core STATIC src/store.cpp src/file.cpp)

add_executable(kvlog src/main.cpp)
target_link_libraries(kvlog PRIVATE kvlog_core)

add_executable(kvlog_tests tests/tests.cpp)
target_link_libraries(kvlog_tests PRIVATE kvlog_core)

enable_testing()
add_test(NAME tests COMMAND kvlog_tests)

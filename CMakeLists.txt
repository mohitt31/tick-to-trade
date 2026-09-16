cmake_minimum_required(VERSION 3.25)
project(tick_to_trade LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TTT_MUTANTS "Compile planted-bug switches (never in release)" OFF)
option(TTT_WERROR "Treat warnings as errors" ON)
option(TTT_BUILD_TESTS "Build tests" ON)

# Project 1's checking levels, shared so that its book and this project's code
# are always built at the same level. 0 release, 1 O(1) checks, 2 also O(n).
set(ITCH_INVARIANT_LEVEL "2" CACHE STRING "Runtime invariant checking level (0/1/2)")
set_property(CACHE ITCH_INVARIANT_LEVEL PROPERTY STRINGS 0 1 2)

add_library(ttt_warnings INTERFACE)
target_compile_options(ttt_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast -Wnon-virtual-dtor -Wimplicit-fallthrough -Wformat=2
  $<$<BOOL:${TTT_WERROR}>:-Werror>)

# gtest macros trip -Wconversion and friends.
add_library(ttt_test_warnings INTERFACE)
target_compile_options(ttt_test_warnings INTERFACE
  -Wall -Wextra $<$<BOOL:${TTT_WERROR}>:-Werror>)

add_library(ttt_options INTERFACE)
target_compile_definitions(ttt_options INTERFACE
  ITCH_INVARIANT_LEVEL=${ITCH_INVARIANT_LEVEL}
  $<$<BOOL:${TTT_MUTANTS}>:TTT_MUTANTS>)

# Project 1, pinned as a submodule. Only its headers and its one source file are
# used. Its own CMakeLists also builds its apps, benchmarks and tests, and turns
# on -march=native, none of which belongs in this build.
set(ITCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/itch-exchange)
if(NOT EXISTS ${ITCH_DIR}/include/itch/core/types.hpp)
  message(FATAL_ERROR "third_party/itch-exchange is empty: git submodule update --init")
endif()
add_library(itch STATIC ${ITCH_DIR}/src/assert.cpp)
target_include_directories(itch PUBLIC ${ITCH_DIR}/include)
target_link_libraries(itch PUBLIC ttt_options)

add_subdirectory(src)

if(TTT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

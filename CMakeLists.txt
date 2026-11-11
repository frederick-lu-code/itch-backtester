cmake_minimum_required(VERSION 3.25)
project(itchbt VERSION 0.1.0 DESCRIPTION "Deterministic ITCH 5.0 backtester" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ITCHBT_BUILD_TESTS "Build unit, property and integration tests" ON)
option(ITCHBT_BUILD_BENCH "Build benchmarks" OFF)
option(ITCHBT_FUZZ "Build libFuzzer targets (needs upstream Clang)" OFF)
option(ITCHBT_NATIVE "Tune optimised builds for the build machine's CPU" OFF)
option(ITCHBT_TIMING "Compile in per-message latency timing" OFF)
set(ITCHBT_SANITIZE "" CACHE STRING "Sanitizers to enable: empty, 'address,undefined' or 'thread'")

# Sanitizer and CPU flags are global on purpose: GoogleTest must be built with the same
# instrumentation as our code or TSan and ASan report false positives across the boundary.
if(ITCHBT_SANITIZE)
    add_compile_options(-fsanitize=${ITCHBT_SANITIZE} -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${ITCHBT_SANITIZE})
endif()

if(ITCHBT_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "ITCHBT_FUZZ needs upstream Clang: libFuzzer does not ship with "
                            "${CMAKE_CXX_COMPILER_ID}.")
    endif()
    # Coverage instrumentation everywhere; only the fuzz targets link the fuzzer runtime.
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

if(ITCHBT_NATIVE)
    include(CheckCXXCompilerFlag)
    # -march=native is the x86-64 spelling; Clang on arm64 wants -mcpu=native.
    check_cxx_compiler_flag(-march=native ITCHBT_HAS_MARCH_NATIVE)
    check_cxx_compiler_flag(-mcpu=native ITCHBT_HAS_MCPU_NATIVE)
    if(ITCHBT_HAS_MARCH_NATIVE)
        add_compile_options(-march=native)
    elseif(ITCHBT_HAS_MCPU_NATIVE)
        add_compile_options(-mcpu=native)
    endif()
endif()

# Warning set for first-party targets only. Relaxing it needs the owner's approval.
add_library(itchbt_warnings INTERFACE)
target_compile_options(itchbt_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                 -Werror)

include(cmake/Dependencies.cmake)

# Most of the engine is templates in include/; src/ holds what does not need to inline.
add_library(itchbt STATIC src/itch/reader.cpp src/record/recorder.cpp src/record/schema.cpp
                          src/sim/config.cpp
                          src/util/platform.cpp src/util/sha256.cpp)
add_library(itchbt::itchbt ALIAS itchbt)
target_include_directories(itchbt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(itchbt PUBLIC cxx_std_20)
target_link_libraries(itchbt PRIVATE itchbt_warnings tomlplusplus::tomlplusplus)
if(ITCHBT_TIMING)
    target_compile_definitions(itchbt PUBLIC ITCHBT_TIMING=1)
endif()

# Recorded in run.json. Captured at configure time, so re-run cmake after committing if the
# exact commit matters.
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE ITCHBT_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT ITCHBT_GIT_COMMIT)
    set(ITCHBT_GIT_COMMIT "unknown")
endif()

add_subdirectory(apps)
if(ITCHBT_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(ITCHBT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

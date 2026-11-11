# Third-party dependencies, pinned to release tags. Adding one needs the owner's approval.
include(FetchContent)

# SYSTEM keeps our -Werror warning set off third-party headers.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
    SYSTEM)

# Google Benchmark is declared here so the pins live in one place; it is fetched only when
# benchmarks are built.
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE
    SYSTEM)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
    SYSTEM)

if(ITCHBT_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Header-only; used by the config loader.
FetchContent_MakeAvailable(tomlplusplus)

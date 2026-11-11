function(itchbt_add_bench name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE itchbt::itchbt itchbt_warnings)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

itchbt_add_bench(parse_bench parse_bench.cpp)
itchbt_add_bench(book_bench book_bench.cpp)

# The prefetch experiment from the optimisation log, kept buildable so it can be re-measured.
itchbt_add_bench(book_bench_prefetch book_bench.cpp)
target_compile_definitions(book_bench_prefetch PRIVATE ITCHBT_BENCH_PREFETCH=1)

# micro_bench replays synthetic streams, so it needs the test support library.
if(ITCHBT_BUILD_TESTS)
    itchbt_add_bench(micro_bench micro_bench.cpp)
    target_link_libraries(micro_bench PRIVATE benchmark::benchmark itchbt_test_support)
    target_include_directories(micro_bench PRIVATE ${PROJECT_SOURCE_DIR}/strategies)
endif()

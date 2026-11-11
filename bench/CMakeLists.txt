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

function(itchbt_add_bench name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE itchbt::itchbt itchbt_warnings)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

itchbt_add_bench(parse_bench parse_bench.cpp)
itchbt_add_bench(book_bench book_bench.cpp)

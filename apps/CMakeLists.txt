function(itchbt_add_app name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE itchbt::itchbt itchbt_warnings)
endfunction()

itchbt_add_app(itch-stats itch_stats.cpp)
itchbt_add_app(book-dump book_dump.cpp)
itchbt_add_app(book-check book_check.cpp)

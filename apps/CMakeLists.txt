function(itchbt_add_app name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE itchbt::itchbt itchbt_warnings)
endfunction()

itchbt_add_app(itch-stats itch_stats.cpp)
itchbt_add_app(book-dump book_dump.cpp)
itchbt_add_app(book-check book_check.cpp)

itchbt_add_app(backtest backtest.cpp)
target_include_directories(backtest PRIVATE ${PROJECT_SOURCE_DIR}/strategies)
target_compile_definitions(backtest PRIVATE ITCHBT_GIT_COMMIT="${ITCHBT_GIT_COMMIT}")

add_executable(parser_fuzz parser_fuzz.cpp)
target_link_libraries(parser_fuzz PRIVATE itchbt::itchbt itchbt_warnings)
target_link_options(parser_fuzz PRIVATE -fsanitize=fuzzer)

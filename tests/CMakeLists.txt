include(GoogleTest)

# Test-only helpers shared by unit, property and integration tests.
add_library(itchbt_test_support STATIC support/itch_writer.cpp)
target_include_directories(itchbt_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                                      ${CMAKE_CURRENT_SOURCE_DIR}/unit)
target_link_libraries(itchbt_test_support PUBLIC itchbt::itchbt PRIVATE itchbt_warnings)

add_executable(unit_tests unit/book_test.cpp unit/directory_test.cpp unit/endian_test.cpp unit/itch_writer_test.cpp unit/parser_test.cpp
                          unit/reader_test.cpp unit/util_test.cpp)
target_link_libraries(unit_tests PRIVATE itchbt_test_support itchbt_warnings GTest::gmock_main)
gtest_discover_tests(unit_tests)

if(ITCHBT_FUZZ)
    add_subdirectory(fuzz)
endif()

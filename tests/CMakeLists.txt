include(GoogleTest)

# Test-only helpers shared by unit, property and integration tests.
add_library(itchbt_test_support STATIC support/itch_writer.cpp)
target_include_directories(itchbt_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                                      ${CMAKE_CURRENT_SOURCE_DIR}/unit)
target_link_libraries(itchbt_test_support PUBLIC itchbt::itchbt PRIVATE itchbt_warnings)

add_executable(
    unit_tests
    unit/book_test.cpp
    unit/config_test.cpp
    unit/directory_test.cpp
    unit/endian_test.cpp
    unit/event_queue_test.cpp
    unit/exchange_test.cpp
    unit/itch_writer_test.cpp
    unit/order_state_test.cpp
    unit/parser_test.cpp
    unit/reader_test.cpp
    unit/recorder_test.cpp
    unit/risk_test.cpp
    unit/sha256_test.cpp
    unit/spsc_ring_test.cpp
    unit/util_test.cpp)
target_link_libraries(unit_tests PRIVATE itchbt_test_support itchbt_warnings GTest::gmock_main)
target_compile_definitions(unit_tests PRIVATE ITCHBT_CONFIG_DIR="${PROJECT_SOURCE_DIR}/configs")
gtest_discover_tests(unit_tests)

add_executable(property_tests property/book_property_test.cpp)
target_link_libraries(property_tests PRIVATE itchbt_test_support itchbt_warnings GTest::gmock_main)
gtest_discover_tests(property_tests)

add_executable(integration_tests integration/engine_test.cpp integration/strategies_test.cpp)
target_link_libraries(integration_tests PRIVATE itchbt_test_support itchbt_warnings
                                                GTest::gmock_main)
target_include_directories(integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/strategies)
gtest_discover_tests(integration_tests)

if(ITCHBT_FUZZ)
    add_subdirectory(fuzz)
endif()

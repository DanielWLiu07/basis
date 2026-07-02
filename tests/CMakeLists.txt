include(GoogleTest)

add_executable(basis_tests
  test_order_book.cpp
  test_unified_book.cpp
  test_kalshi_parser.cpp
  test_polymarket_parser.cpp
)
target_link_libraries(basis_tests PRIVATE basis_feed GTest::gtest_main)

gtest_discover_tests(basis_tests)

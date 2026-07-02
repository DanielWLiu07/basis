include(GoogleTest)

add_executable(basis_tests
  test_order_book.cpp
  test_unified_book.cpp
  test_kalshi_parser.cpp
  test_polymarket_parser.cpp
  test_feed_log.cpp
  test_contract_registry.cpp
  test_normalizer.cpp
)
target_link_libraries(basis_tests PRIVATE basis_feed basis_normalize GTest::gtest_main)
# Lets tests load checked-in configs relative to the repo, not the build dir.
target_compile_definitions(basis_tests PRIVATE
  BASIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gtest_discover_tests(basis_tests)

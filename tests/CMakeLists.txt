include(GoogleTest)

add_executable(basis_tests
  test_order_book.cpp
)
target_link_libraries(basis_tests PRIVATE basis_model GTest::gtest_main)

gtest_discover_tests(basis_tests)

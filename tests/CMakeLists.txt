include(GoogleTest)

add_executable(basis_tests
  test_order_book.cpp
  test_unified_book.cpp
  test_kalshi_parser.cpp
  test_polymarket_parser.cpp
  test_feed_log.cpp
  test_contract_registry.cpp
  test_normalizer.cpp
  test_divergence.cpp
  test_lead_lag.cpp
  test_session.cpp
  test_latency_recorder.cpp
  test_replay_harness.cpp
  test_synth_generator.cpp
  test_counting_resource.cpp
)
target_link_libraries(basis_tests PRIVATE basis_bench GTest::gtest_main)
if(BASIS_ENABLE_BDE)
  target_sources(basis_tests PRIVATE test_bde_arena.cpp)
  target_link_libraries(basis_tests PRIVATE basis_alloc_bde)
endif()
# Lets tests load checked-in configs relative to the repo, not the build dir.
target_compile_definitions(basis_tests PRIVATE
  BASIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gtest_discover_tests(basis_tests)

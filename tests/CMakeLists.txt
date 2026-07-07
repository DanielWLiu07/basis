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
  test_event_study.cpp
  test_lead_lag.cpp
  test_session.cpp
  test_latency_recorder.cpp
  test_replay_harness.cpp
  test_synth_generator.cpp
  test_counting_resource.cpp
  test_sha1.cpp
  test_bounded_queue.cpp
)
target_link_libraries(basis_tests PRIVATE basis_bench GTest::gtest_main)
if(BASIS_ENABLE_BDE)
  target_sources(basis_tests PRIVATE test_bde_arena.cpp)
  target_link_libraries(basis_tests PRIVATE basis_alloc_bde)
endif()
if(BASIS_ENABLE_NET)
  target_sources(basis_tests PRIVATE
    test_kalshi_auth.cpp
    test_reconnect.cpp
    support/test_tls.cpp
    support/flaky_ws_server.cpp
  )
  target_include_directories(basis_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(basis_tests PRIVATE basis_feed_live)
endif()
# Lets tests load checked-in configs relative to the repo, not the build dir.
target_compile_definitions(basis_tests PRIVATE
  BASIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gtest_discover_tests(basis_tests)

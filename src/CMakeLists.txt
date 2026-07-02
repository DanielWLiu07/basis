# Layering is enforced by what each library links. See PLAN.md.
#   basis_core   <- nothing project-specific
#   basis_model  <- basis_core
#   basis_feed      <- basis_model (+ simdjson)
#   basis_normalize <- basis_model
#   basis_analytics <- basis_model
#   basis_api       <- basis_model
#   basis (exe)     <- everything above

add_library(basis_core
  core/logger.cpp
)
target_include_directories(basis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(basis_core PUBLIC basis_warnings basis_sanitize)

add_library(basis_model
  model/order_book.cpp
  model/unified_book.cpp
)
target_link_libraries(basis_model PUBLIC basis_core)

add_library(basis_feed
  feed/kalshi_parser.cpp
  feed/polymarket_parser.cpp
  feed/feed_log.cpp
)
target_link_libraries(basis_feed PUBLIC basis_model simdjson)

add_library(basis_normalize
  normalize/contract_registry.cpp
  normalize/normalizer.cpp
)
target_link_libraries(basis_normalize PUBLIC basis_model)

add_library(basis_analytics
  analytics/lead_lag.cpp
)
target_link_libraries(basis_analytics PUBLIC basis_model)

add_library(basis_api
  api/in_process_session.cpp
)
target_link_libraries(basis_api PUBLIC basis_model)

add_executable(basis main.cpp)
target_link_libraries(basis PRIVATE basis_feed basis_normalize basis_analytics
  basis_api)

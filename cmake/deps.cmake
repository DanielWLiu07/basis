include(FetchContent)

# Phase 0: GoogleTest only. Reliable, quick, and the only network dependency
# the scaffold needs.
if(BASIS_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  foreach(_gt_target gtest gtest_main gmock gmock_main)
    if(TARGET ${_gt_target})
      get_target_property(_gt_includes ${_gt_target} INTERFACE_INCLUDE_DIRECTORIES)
      set_target_properties(${_gt_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_gt_includes}")
    endif()
  endforeach()
endif()

# simdjson is always on: the feed parsers are the heart of the offline replay
# pipeline, not just the live path, so JSON parsing is a core dependency.
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        v3.10.1)
FetchContent_MakeAvailable(simdjson)
# Third-party headers are not ours to fix; keep them out of our -W set.
get_target_property(_simdjson_includes simdjson INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(simdjson PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_simdjson_includes}")

# Phase 1: live WSS feeds. Wired when BASIS_ENABLE_NET is set.
#   - OpenSSL (system) for wss://
#   - Boost.Beast for the WebSocket client
if(BASIS_ENABLE_NET)
  message(STATUS "[basis] NET enabled: add OpenSSL + Boost.Beast here (Phase 1)")
  # find_package(OpenSSL REQUIRED)
  # find_package(Boost 1.80 REQUIRED)
endif()

# Phase 3: Bloomberg BDE allocators (bdlma) for the normalize hot path.
# Scope to bdlma only; do not pull all of bsl/bde.
if(BASIS_ENABLE_BDE)
  message(STATUS "[basis] BDE enabled: add bloomberg/bde (bdlma) here (Phase 3)")
endif()

# Phase 4: HdrHistogram for ingest-to-signal latency percentiles.
if(BASIS_ENABLE_BENCH)
  message(STATUS "[basis] BENCH enabled: add HdrHistogram_c here (Phase 4)")
endif()

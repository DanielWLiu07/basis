include(FetchContent)

# Phase 0: GoogleTest only. Reliable, quick, and the only network dependency
# the scaffold needs.
if(BASIS_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# Phase 1: live WSS feeds + JSON parsing. Wired when BASIS_ENABLE_NET is set.
#   - OpenSSL (system) for wss://
#   - Boost.Beast for the WebSocket client
#   - simdjson for low-allocation parsing
if(BASIS_ENABLE_NET)
  message(STATUS "[basis] NET enabled: add OpenSSL + Boost.Beast + simdjson here (Phase 1)")
  # find_package(OpenSSL REQUIRED)
  # find_package(Boost 1.80 REQUIRED)
  # FetchContent_Declare(simdjson GIT_REPOSITORY https://github.com/simdjson/simdjson.git GIT_TAG v3.10.1)
  # FetchContent_MakeAvailable(simdjson)
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

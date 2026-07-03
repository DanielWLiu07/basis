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

# Live WSS feeds, behind BASIS_ENABLE_NET: OpenSSL (system) for wss:// and
# Boost.Beast (header-only, system Boost) for the WebSocket client. System
# packages rather than FetchContent: Boost is far too heavy to vendor.
if(BASIS_ENABLE_NET)
  find_package(OpenSSL REQUIRED)
  find_package(Boost 1.80 REQUIRED CONFIG)
  find_package(Threads REQUIRED)
  message(STATUS "[basis] NET enabled: Boost ${Boost_VERSION}, OpenSSL ${OPENSSL_VERSION}")
endif()

# Phase 3: Bloomberg BDE allocators (bdlma) for the hot path. Scope to
# bdlma only; do not pull all of bsl/bde.
#
# Found as plain libraries rather than via BDE's CMake packages: Homebrew's
# bdl package config references a libpcre2-posix package that nothing
# provides, and the bdlma slice we use pulls no pcre2, decimal, or ryu
# objects out of the static archives anyway. On macOS: brew install bde.
# Elsewhere: build bloomberg/bde and point CMAKE_PREFIX_PATH at the prefix.
if(BASIS_ENABLE_BDE)
  find_path(BASIS_BDE_INCLUDE_DIR bdlma_sequentialallocator.h
    HINTS /opt/homebrew/opt/bde/include /usr/local/opt/bde/include)
  find_library(BASIS_BDL_LIBRARY bdl
    HINTS /opt/homebrew/opt/bde/lib /usr/local/opt/bde/lib)
  find_library(BASIS_BSL_LIBRARY bsl
    HINTS /opt/homebrew/opt/bde/lib /usr/local/opt/bde/lib)
  if(NOT BASIS_BDE_INCLUDE_DIR OR NOT BASIS_BDL_LIBRARY OR NOT BASIS_BSL_LIBRARY)
    message(FATAL_ERROR "[basis] BASIS_ENABLE_BDE=ON but BDE was not found. "
      "On macOS: brew install bde. Otherwise build bloomberg/bde and set "
      "CMAKE_PREFIX_PATH to its install prefix.")
  endif()
  message(STATUS "[basis] BDE enabled: ${BASIS_BDL_LIBRARY}")
endif()

# Phase 4: HdrHistogram for ingest-to-signal latency percentiles.
if(BASIS_ENABLE_BENCH)
  message(STATUS "[basis] BENCH enabled: add HdrHistogram_c here (Phase 4)")
endif()

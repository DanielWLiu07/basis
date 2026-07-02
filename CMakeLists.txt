cmake_minimum_required(VERSION 3.20)
project(basis VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Phase-gated features. Phase 0 builds with only GoogleTest so the first
# configure is fast and offline-friendly. See PLAN.md "Dependency onboarding".
option(BASIS_BUILD_TESTS      "Build unit tests (fetches GoogleTest)"        ON)
option(BASIS_ENABLE_NET       "Enable live feeds (Boost.Beast + simdjson)"   OFF)
option(BASIS_ENABLE_BDE       "Enable Bloomberg BDE bdlma allocators"        OFF)
option(BASIS_ENABLE_BENCH     "Build the replay benchmark (HdrHistogram)"    OFF)
option(BASIS_SANITIZE         "Build with ASan + UBSan"                      OFF)
option(BASIS_SANITIZE_THREAD  "Build with ThreadSanitizer"                   OFF)

include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/deps.cmake)

add_subdirectory(src)

if(BASIS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

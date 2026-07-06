# libFuzzer targets, one per untrusted-input surface. The libraries are
# already instrumented by basis_sanitize (fuzzer-no-link + ASan + UBSan
# under BASIS_ENABLE_FUZZ); the targets here add the fuzzing runtime.
#
#   cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBASIS_ENABLE_FUZZ=ON
#   cmake --build build-fuzz -j
#   ./build-fuzz/fuzz/fuzz_kalshi_parser fuzz/corpus/kalshi
#
# CI runs each target for a fixed budget on every commit; local runs can
# go as long as they like. Corpus seeds are real wire messages.

function(basis_fuzz_target name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE basis_feed basis_normalize)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

basis_fuzz_target(fuzz_kalshi_parser)
basis_fuzz_target(fuzz_polymarket_parser)
basis_fuzz_target(fuzz_contract_registry)

# Fuzzing

One libFuzzer target per untrusted-input surface: the two venue wire
parsers and the contracts registry parser. The parsers' contract is
total: any byte sequence is Ok, Ignored, or Malformed, never a crash, an
overflow, or a fabricated price level.

```
cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBASIS_ENABLE_FUZZ=ON
cmake --build build-fuzz -j
./build-fuzz/fuzz/fuzz_kalshi_parser -max_len=4096 fuzz/corpus/kalshi
```

CI runs every target for 100k executions per commit on Linux with full
ASan and UBSan checking. Local deep runs (2M executions per target at
last count) have produced zero findings in project code.

macOS note: Apple's clang does not ship the libFuzzer runtime, so build
with Homebrew LLVM (`CC=/opt/homebrew/opt/llvm/bin/clang ...`). That
runtime is itself not container-annotation instrumented, which makes
ASan report a container-overflow inside `fuzzer::ReadCorpora` (runtime
frames only, none of ours) once a corpus directory grows past a few
dozen files. For local runs, disable that one check with
`ASAN_OPTIONS=detect_container_overflow=0`; CI on Linux keeps it on.

`corpus/` holds hand-written seeds only, one per message shape the wire
actually produces; fuzzer-grown entries are disposable and stay out of
the repo.

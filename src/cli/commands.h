#pragma once

#include <string_view>
#include <vector>

namespace basis::cli {

// One function per subcommand. Each takes the arguments after the
// subcommand name and returns the process exit code.
//
// These used to be a single 1,400-line main.cpp, which made the file three
// times the size of anything else in the repo and put a matching-engine
// microbenchmark, a book reconciliation checker, and a live WebSocket
// recorder in one translation unit whose only common factor was that a
// user can ask for them from a shell.
//
// They are grouped by what they need rather than by what they do:
// bench_commands.cpp runs in-process microbenchmarks, analysis_commands.cpp
// reads captures off disk, and live_commands.cpp opens sockets and only
// exists in a BASIS_ENABLE_NET build.

// bench_commands.cpp
int run_lob_bench_cmd(const std::vector<std::string_view>& args);
int run_fanout_bench_cmd(const std::vector<std::string_view>& args);
int run_ingest_bench_cmd(const std::vector<std::string_view>& args);

// analysis_commands.cpp
int run_synth(const std::vector<std::string_view>& args);
int run_replay(const std::vector<std::string_view>& args);
int run_xvenue_lead_cmd(const std::vector<std::string_view>& args);
int run_book_verify_cmd(const std::vector<std::string_view>& args);

#ifdef BASIS_HAS_NET
// live_commands.cpp
int run_record(const std::vector<std::string_view>& args);
int run_live(const std::vector<std::string_view>& args);
#endif

}  // namespace basis::cli

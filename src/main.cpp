// Composition root: map a subcommand name onto a function and get out of
// the way. Everything the commands themselves do lives in cli/.
#include <string_view>
#include <vector>

#include "cli/commands.h"
#include "cli/usage.h"

int main(int argc, char** argv) {
  using namespace basis::cli;

  std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.empty()) return usage();

  const auto command = args[0];
  const std::vector<std::string_view> rest(args.begin() + 1, args.end());
  if (command == "lob-bench") return run_lob_bench_cmd(rest);
  if (command == "fanout-bench") return run_fanout_bench_cmd(rest);
  if (command == "ingest-bench") return run_ingest_bench_cmd(rest);
  if (command == "xvenue-lead") return run_xvenue_lead_cmd(rest);
  if (command == "book-verify") return run_book_verify_cmd(rest);
  if (command == "synth") return run_synth(rest);
  if (command == "replay") return run_replay(rest);
#ifdef BASIS_HAS_NET
  if (command == "record") return run_record(rest);
  if (command == "live") return run_live(rest);
#endif
  return usage();
}

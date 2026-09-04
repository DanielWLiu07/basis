#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basis::cli {

// Argument handling shared by every subcommand. Flags are `--name value`
// pairs following the positional argument, which is the whole grammar:
// there is no getopt here because a benchmark driver that needs one has
// outgrown being a benchmark driver.

// printf's %llu wants unsigned long long; our counters are uint64_t. One
// short name instead of a 40-character cast at every use site.
inline unsigned long long u(std::uint64_t v) {
  return static_cast<unsigned long long>(v);
}

std::optional<std::int64_t> parse_int(std::string_view s);

// nullopt distinguishes "present but malformed" from "absent", so a
// command can reject `--steps banana` instead of silently defaulting.
std::optional<std::int64_t> flag_value(const std::vector<std::string_view>& args,
                                       std::string_view name,
                                       std::int64_t fallback);

std::string flag_string(const std::vector<std::string_view>& args,
                        std::string_view name, std::string fallback);

// Real-valued flags (--speed 2500, --speed 0.5). Returns the fallback
// when the flag is absent OR unparseable: the callers that use this take
// a rate, and every rate they accept is validated against its own bounds
// right after, so a malformed value is caught there with a message that
// names the flag.
// Returns nullopt when the flag is present but its value is not a
// complete number, or when it is present with no value at all - the same
// contract flag_value has, and for the same reason. It used to return the
// fallback in both cases, so `--speed banana` silently produced an
// unpaced replay: the one measurement that mode exists to correct.
std::optional<double> flag_double(const std::vector<std::string_view>& args,
                   std::string_view name, double fallback);

// Comma-separated flag values (--binance btcusdt,ethusdt).
std::vector<std::string> split_csv(std::string_view text);

bool has_flag(const std::vector<std::string_view>& args, std::string_view name);

}  // namespace basis::cli

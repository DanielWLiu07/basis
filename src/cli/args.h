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

// Comma-separated flag values (--binance btcusdt,ethusdt).
std::vector<std::string> split_csv(std::string_view text);

bool has_flag(const std::vector<std::string_view>& args, std::string_view name);

}  // namespace basis::cli

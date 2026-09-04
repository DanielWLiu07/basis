#include "cli/args.h"

#include <charconv>
#include <stdexcept>
#include <string>

namespace basis::cli {

std::optional<std::int64_t> parse_int(std::string_view s) {
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
  return value;
}

// Flags are --name value pairs after the positional argument.
std::optional<std::int64_t> flag_value(const std::vector<std::string_view>& args,
                                       std::string_view name,
                                       std::int64_t fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) {
      return parse_int(args[i + 1]);  // nullopt: malformed number
    }
  }
  return fallback;
}

std::string flag_string(const std::vector<std::string_view>& args,
                        std::string_view name, std::string fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) return std::string(args[i + 1]);
  }
  return fallback;
}

std::optional<double> flag_double(const std::vector<std::string_view>& args,
                                  std::string_view name, double fallback) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] != name) continue;
    // Present as the final argument, so there is no value to read. Silently
    // falling back here is what let `--speed` with no number look like a
    // deliberate unpaced run.
    if (i + 1 >= args.size()) return std::nullopt;
    const std::string text(args[i + 1]);
    try {
      std::size_t consumed = 0;
      const double v = std::stod(text, &consumed);
      if (consumed != text.size()) return std::nullopt;  // trailing junk
      return v;
    } catch (...) {
      return std::nullopt;
    }
  }
  return fallback;
}

// Comma-separated flag values (--binance btcusdt,ethusdt). Empty entries
// are dropped rather than turned into an empty subscription.
std::vector<std::string> split_csv(std::string_view text) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto comma = text.find(',');
    auto item = text.substr(0, comma);
    if (!item.empty()) out.emplace_back(item);
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return out;
}

// Presence-only flag: true if `name` appears anywhere in args.
bool has_flag(const std::vector<std::string_view>& args,
              std::string_view name) {
  for (const auto& arg : args) {
    if (arg == name) return true;
  }
  return false;
}

}  // namespace basis::cli

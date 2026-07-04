#include "normalize/contract_registry.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace basis::normalize {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

void set_error(std::string* error, int line, std::string_view message) {
  if (error) {
    std::ostringstream oss;
    oss << "contracts.toml line " << line << ": " << message;
    *error = oss.str();
  }
}

// One [[event]] table under construction.
struct PendingEvent {
  int line = 0;  // where the table started, for error reporting
  std::string id;
  std::string kalshi;
  std::string polymarket_token;
  std::string polymarket_no_token;
  bool open = false;
};

}  // namespace

std::optional<TomlContractRegistry> TomlContractRegistry::load(
    const std::string& path, std::string* error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    if (error) *error = "cannot open " + path;
    return std::nullopt;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str(), error);
}

std::optional<TomlContractRegistry> TomlContractRegistry::parse(
    std::string_view text, std::string* error) {
  TomlContractRegistry registry;
  PendingEvent event;

  const auto flush_event = [&]() -> bool {
    if (!event.open) return true;
    if (event.id.empty()) {
      set_error(error, event.line, "[[event]] has no id");
      return false;
    }
    if (!event.kalshi.empty()) {
      if (!registry.kalshi_to_event_.emplace(event.kalshi, event.id).second) {
        set_error(error, event.line,
                  "kalshi market '" + event.kalshi + "' mapped twice");
        return false;
      }
      registry.kalshi_tickers_.push_back(event.kalshi);
    }
    if (!event.polymarket_token.empty()) {
      if (!registry.polymarket_to_event_
               .emplace(event.polymarket_token, event.id)
               .second) {
        set_error(error, event.line,
                  "polymarket token '" + event.polymarket_token +
                      "' mapped twice");
        return false;
      }
      registry.polymarket_tokens_.push_back(event.polymarket_token);
    }
    if (!event.polymarket_no_token.empty()) {
      // The NO token maps to the same event but is folded on arrival, so
      // it shares the duplicate check with every other Polymarket id. It
      // is not a subscribe key: the venue pushes the NO book unasked for
      // any subscribed YES token.
      if (!registry.polymarket_to_event_
               .emplace(event.polymarket_no_token, event.id)
               .second) {
        set_error(error, event.line,
                  "polymarket token '" + event.polymarket_no_token +
                      "' mapped twice");
        return false;
      }
      registry.polymarket_no_tokens_.insert(event.polymarket_no_token);
    }
    registry.event_ids_.push_back(event.id);
    event = PendingEvent{};
    return true;
  };

  int line_no = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto eol = text.find('\n', pos);
    const auto raw = text.substr(
        pos, eol == std::string_view::npos ? std::string_view::npos
                                           : eol - pos);
    pos = (eol == std::string_view::npos) ? text.size() + 1 : eol + 1;
    ++line_no;

    const auto line = trim(raw);
    if (line.empty() || line.front() == '#') continue;

    if (line == "[[event]]") {
      if (!flush_event()) return std::nullopt;
      event.open = true;
      event.line = line_no;
      continue;
    }

    const auto eq = line.find('=');
    if (eq == std::string_view::npos) {
      set_error(error, line_no, "expected key = \"value\"");
      return std::nullopt;
    }
    if (!event.open) {
      set_error(error, line_no, "key outside an [[event]] table");
      return std::nullopt;
    }
    const auto key = trim(line.substr(0, eq));
    const auto value_raw = trim(line.substr(eq + 1));
    if (value_raw.size() < 2 || value_raw.front() != '"' ||
        value_raw.back() != '"') {
      set_error(error, line_no, "value must be a quoted string");
      return std::nullopt;
    }
    const auto value = value_raw.substr(1, value_raw.size() - 2);

    if (key == "id") {
      event.id = std::string(value);
    } else if (key == "kalshi") {
      event.kalshi = std::string(value);
    } else if (key == "polymarket_token") {
      event.polymarket_token = std::string(value);
    } else if (key == "polymarket_no_token") {
      event.polymarket_no_token = std::string(value);
    }
    // Other keys (description, polymarket_market, ...) are documentation.
  }

  if (!flush_event()) return std::nullopt;
  return registry;
}

std::optional<std::string_view> TomlContractRegistry::event_id(
    model::Venue venue, std::string_view market) const {
  const auto& map = (venue == model::Venue::Kalshi) ? kalshi_to_event_
                                                    : polymarket_to_event_;
  const auto it = map.find(market);
  if (it == map.end()) return std::nullopt;
  return std::string_view(it->second);
}

bool TomlContractRegistry::is_polymarket_no(std::string_view market) const {
  return polymarket_no_tokens_.find(market) != polymarket_no_tokens_.end();
}

}  // namespace basis::normalize

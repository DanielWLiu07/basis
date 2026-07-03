#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/hash.h"
#include "model/types.h"

namespace basis::normalize {

// Maps a venue-native market id to a venue-neutral event id, so the same
// real-world outcome on Kalshi and Polymarket lands in one UnifiedBook.
// Config-driven (configs/contracts.toml); fuzzy auto-matching is a stretch.
class ContractRegistry {
 public:
  virtual ~ContractRegistry() = default;

  // Canonical event id for a (venue, market) pair, if one is mapped. The
  // view aliases storage owned by the registry and stays valid for the
  // registry's lifetime; returning a view keeps the per-delta lookup free
  // of allocation.
  virtual std::optional<std::string_view> event_id(
      model::Venue venue, std::string_view market) const = 0;
};

// Registry backed by the contracts.toml subset:
//
//   [[event]]
//   id = "fed-cut-2026-09"
//   kalshi = "FED-26SEP-CUT"           (Kalshi market_ticker)
//   polymarket_token = "7132107..."    (YES-outcome asset id)
//
// Only [[event]] tables of key = "quoted string" pairs and full-line #
// comments are understood, which is all the registry file uses; a TOML
// library stays out of the dependency set. Unknown keys are ignored,
// malformed lines fail loudly with their line number.
class TomlContractRegistry final : public ContractRegistry {
 public:
  static std::optional<TomlContractRegistry> load(const std::string& path,
                                                  std::string* error = nullptr);
  static std::optional<TomlContractRegistry> parse(std::string_view text,
                                                   std::string* error = nullptr);

  std::optional<std::string_view> event_id(
      model::Venue venue, std::string_view market) const override;

  const std::vector<std::string>& event_ids() const { return event_ids_; }

  // Subscribe keys for the live feeds, in file order.
  const std::vector<std::string>& kalshi_tickers() const {
    return kalshi_tickers_;
  }
  const std::vector<std::string>& polymarket_tokens() const {
    return polymarket_tokens_;
  }

 private:
  TomlContractRegistry() = default;

  core::StringMap<std::string> kalshi_to_event_;
  core::StringMap<std::string> polymarket_to_event_;
  std::vector<std::string> event_ids_;
  std::vector<std::string> kalshi_tickers_;
  std::vector<std::string> polymarket_tokens_;
};

}  // namespace basis::normalize

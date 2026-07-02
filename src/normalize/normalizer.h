#pragma once

#include <optional>
#include <string>

#include "model/book_delta.h"

namespace basis::normalize {

// Maps a venue-native market id to a venue-neutral event id, so the same
// real-world outcome on Kalshi and Polymarket lands in one UnifiedBook.
// Phase 3 starts config-driven (configs/contracts.toml); fuzzy auto-matching
// is a stretch.
class ContractRegistry {
 public:
  virtual ~ContractRegistry() = default;

  // Canonical event id for a (venue, market) pair, if one is mapped.
  virtual std::optional<std::string> event_id(model::Venue venue,
                                              const std::string& market) const = 0;
};

}  // namespace basis::normalize

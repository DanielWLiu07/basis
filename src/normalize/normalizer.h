#pragma once

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>

#include "core/hash.h"
#include "model/book_delta.h"
#include "model/unified_book.h"
#include "normalize/contract_registry.h"

namespace basis::normalize {

// Routes canonical deltas into per-event unified books. This is the point
// where venue-native market ids disappear: everything downstream is keyed by
// the registry's venue-neutral event id. Deltas for unmapped markets are
// counted and skipped, never guessed.
class Normalizer {
 public:
  // Fires after a delta has been applied to its event's book, on the
  // caller's thread.
  using Observer = std::function<void(const std::string& event_id,
                                      const model::UnifiedBook& book,
                                      const model::BookDelta& delta)>;

  // `book_mr` backs the per-event order books' level nodes; the default is
  // the global heap. The books outlive any parse arena, so this must be a
  // long-lived resource (a node pool, not a per-message arena).
  explicit Normalizer(const ContractRegistry& registry,
                      std::pmr::memory_resource* book_mr =
                          std::pmr::get_default_resource())
      : registry_(registry), book_mr_(book_mr) {}

  void set_observer(Observer observer) { observer_ = std::move(observer); }

  // True if the delta was mapped to an event and applied.
  bool on_delta(const model::BookDelta& delta);

  const model::UnifiedBook* book(std::string_view event_id) const;

  std::uint64_t unmapped_deltas() const { return unmapped_; }

 private:
  const ContractRegistry& registry_;
  std::pmr::memory_resource* book_mr_;
  Observer observer_;
  core::StringMap<model::UnifiedBook> books_;
  std::uint64_t unmapped_ = 0;
};

}  // namespace basis::normalize

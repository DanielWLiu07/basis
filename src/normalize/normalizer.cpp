#include "normalize/normalizer.h"

namespace basis::normalize {

bool Normalizer::on_delta(const model::BookDelta& delta) {
  const auto event_id = registry_.event_id(delta.venue, delta.market);
  if (!event_id) {
    ++unmapped_;
    return false;
  }
  // Heterogeneous find first: in steady state the event exists, so the
  // common case is one hash probe with no key materialization.
  auto it = books_.find(*event_id);
  if (it == books_.end()) {
    it = books_.emplace(std::string(*event_id), model::UnifiedBook(book_mr_))
             .first;
  }
  auto& book = it->second;

  // A Polymarket NO-token book mirrors its YES book: a NO bid at p is a
  // YES ask at 100 - p, same size (the venue matches a NO buy as a YES
  // sell). Folding puts both wire views into one YES-frame book, the same
  // fold the Kalshi parser applies to its "no" side. Clear carries no
  // level, so it passes through and a NO snapshot rebuilds the whole book
  // in the YES frame.
  if (delta.venue == model::Venue::Polymarket &&
      registry_.is_polymarket_no(delta.market)) {
    model::BookDelta folded = delta;
    if (folded.action != model::Action::Clear) {
      folded.side = (folded.side == model::Side::Bid) ? model::Side::Ask
                                                      : model::Side::Bid;
      folded.price_cents = 100 - folded.price_cents;
    }
    book.apply(folded);
    if (observer_) observer_(it->first, book, folded);
    return true;
  }

  book.apply(delta);
  if (observer_) observer_(it->first, book, delta);
  return true;
}

const model::UnifiedBook* Normalizer::book(std::string_view event_id) const {
  const auto it = books_.find(event_id);
  return it == books_.end() ? nullptr : &it->second;
}

}  // namespace basis::normalize

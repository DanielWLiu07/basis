#include "bench/replay_harness.h"

#include <algorithm>

#include "core/time.h"
#include "feed/feed_log.h"

namespace basis::bench {

ReplayHarness::ReplayHarness(const normalize::ContractRegistry& registry,
                             api::InProcessSession* session,
                             analytics::LeadLagConfig lead_lag_config)
    : registry_(registry),
      session_(session),
      lead_lag_config_(lead_lag_config),
      normalizer_(registry_) {
  normalizer_.set_observer([this](const std::string& event_id,
                                  const model::UnifiedBook& book,
                                  const model::BookDelta& delta) {
    on_event_update(event_id, book, delta);
  });
}

void ReplayHarness::on_event_update(const std::string& event_id,
                                    const model::UnifiedBook& book,
                                    const model::BookDelta& delta) {
  const auto kalshi_mid = book.mid(model::Venue::Kalshi);
  const auto poly_mid = book.mid(model::Venue::Polymarket);

  auto it = analytics_.find(event_id);
  if (it == analytics_.end()) {
    it = analytics_.emplace(event_id, EventAnalytics(lead_lag_config_)).first;
  }
  if (kalshi_mid && poly_mid) {
    it->second.divergence.observe(*kalshi_mid - *poly_mid);
    it->second.lead_lag.observe(*kalshi_mid, *poly_mid, delta.ts_ns);
  }

  if (!session_) return;
  if (kalshi_mid) {
    session_->publish({.event_id = event_id, .field = "kalshi_mid",
                       .value = *kalshi_mid, .ts_ns = delta.ts_ns});
  }
  if (poly_mid) {
    session_->publish({.event_id = event_id, .field = "poly_mid",
                       .value = *poly_mid, .ts_ns = delta.ts_ns});
  }
  if (kalshi_mid && poly_mid) {
    session_->publish({.event_id = event_id, .field = "basis",
                       .value = *kalshi_mid - *poly_mid, .ts_ns = delta.ts_ns});
  }
}

std::optional<ReplayStats> ReplayHarness::run(const std::string& feedlog_path,
                                              std::string* error) {
  feed::FeedLogReader reader(feedlog_path);
  if (!reader.ok()) {
    if (error) *error = "cannot open " + feedlog_path;
    return std::nullopt;
  }

  while (const auto record = reader.next()) {
    ++stats_.records;

    // The measured span: raw bytes in, analytics observed and updates
    // published out. This is the ingest-to-signal latency PLAN.md defines;
    // the feedlog read stays outside it, standing in for the network.
    const auto t0 = time::mono_ns();

    feed::ParseResult parsed;
    if (record->venue == model::Venue::Kalshi) {
      ++stats_.kalshi_messages;
      parsed = kalshi_.parse(record->payload, record->recv_ns);
    } else {
      ++stats_.polymarket_messages;
      parsed = polymarket_.parse(record->payload, record->recv_ns);
    }

    switch (parsed.status) {
      case feed::ParseStatus::Ok:
        stats_.deltas += parsed.deltas.size();
        break;
      case feed::ParseStatus::Ignored:
        ++stats_.ignored;
        break;
      case feed::ParseStatus::Malformed:
        ++stats_.malformed;
        break;
    }
    if (parsed.gap) ++stats_.gaps;

    for (const auto& delta : parsed.deltas) {
      normalizer_.on_delta(delta);
    }

    latency_.record(time::mono_ns() - t0);
  }

  stats_.malformed_lines = reader.malformed_lines();
  stats_.unmapped_deltas = normalizer_.unmapped_deltas();
  stats_.latency = latency_.report();

  stats_.events.clear();
  stats_.events.reserve(analytics_.size());
  for (const auto& [event_id, ea] : analytics_) {
    ReplayStats::EventReport report;
    report.event_id = event_id;
    report.basis_samples = ea.divergence.samples();
    if (report.basis_samples > 0) {
      report.basis_mean = ea.divergence.mean();
      report.basis_min = ea.divergence.min();
      report.basis_max = ea.divergence.max();
      report.basis_last = ea.divergence.last();
    }
    report.lead_lag = ea.lead_lag.estimate();
    stats_.events.push_back(std::move(report));
  }
  std::sort(stats_.events.begin(), stats_.events.end(),
            [](const auto& a, const auto& b) { return a.event_id < b.event_id; });

  return stats_;
}

}  // namespace basis::bench

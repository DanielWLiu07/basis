#include "bench/fanout_bench.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "api/conflating_session.h"
#include "api/in_process_session.h"

namespace basis::bench {

namespace {

using api::ConflatingSession;
using api::InProcessSession;
using api::Update;

// Simulated consumer work: a spin rather than a sleep, so the cost is CPU
// the publisher would actually be charged for in the synchronous model
// instead of a scheduler hand-off that flatters it.
void burn_microseconds(std::uint64_t us) {
  if (us == 0) return;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::microseconds(us);
  while (std::chrono::steady_clock::now() < deadline) {
  }
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

FanoutBenchResult run_fanout_bench(std::uint64_t subscribers,
                                   std::uint64_t slow_subscribers,
                                   std::uint64_t updates,
                                   std::uint64_t slow_handler_us) {
  FanoutBenchResult r;
  r.subscribers = subscribers;
  r.slow_subscribers = slow_subscribers;
  r.updates = updates;
  r.slow_handler_us = slow_handler_us;
  if (subscribers == 0 || updates == 0) return r;

  const std::string kEvent = "fed-2026-07-cut-25";
  const std::string kField = "kalshi_mid";

  // --- Synchronous session: handlers run on the publisher's thread. ---
  {
    InProcessSession sync;
    for (std::uint64_t i = 0; i < subscribers; ++i) {
      const bool slow = i < slow_subscribers;
      sync.subscribe(kEvent, kField, [slow, slow_handler_us](const Update&) {
        if (slow) burn_microseconds(slow_handler_us);
      });
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < updates; ++i) {
      sync.publish(Update{kEvent, kField, static_cast<double>(i),
                          static_cast<std::int64_t>(i)});
    }
    r.sync_publish_ms = ms_since(t0);
  }

  // --- Conflating session: publisher slots values, consumers drain. ---
  {
    ConflatingSession conf;
    std::vector<ConflatingSession::SubscriberId> ids;
    // Last value each subscriber actually saw, for the staleness check.
    std::vector<std::atomic<std::int64_t>> last_seen(subscribers);
    for (std::uint64_t i = 0; i < subscribers; ++i) {
      const auto id = conf.add_subscriber();
      ids.push_back(id);
      const bool slow = i < slow_subscribers;
      conf.subscribe_for(id, kEvent, kField,
                         [&last_seen, i, slow, slow_handler_us](const Update& u) {
                           last_seen[i] = u.ts_ns;
                           if (slow) burn_microseconds(slow_handler_us);
                         });
    }

    std::atomic<bool> running{true};
    std::vector<std::thread> drainers;
    drainers.reserve(subscribers);
    for (std::uint64_t i = 0; i < subscribers; ++i) {
      drainers.emplace_back([&conf, &ids, &running, i] {
        // Back off when there is nothing to take. A spin here would make
        // this benchmark measure CPU contention between the publisher and
        // N idle consumer threads rather than the delivery model, and a
        // real consumer waits rather than spins.
        while (running.load(std::memory_order_relaxed)) {
          if (conf.drain(ids[i]) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
          }
        }
        conf.drain(ids[i]);  // final sweep so the last value lands
      });
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < updates; ++i) {
      conf.publish(Update{kEvent, kField, static_cast<double>(i),
                          static_cast<std::int64_t>(i)});
    }
    r.conflating_publish_ms = ms_since(t0);

    // Let the drainers settle so the staleness measurement reflects the
    // steady state rather than the instant the publisher stopped.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running = false;
    for (auto& t : drainers) t.join();

    const auto stats = conf.stats();
    r.conflated = stats.conflated;
    r.delivered = stats.delivered;
    const std::int64_t final_ts = static_cast<std::int64_t>(updates) - 1;
    for (std::uint64_t i = 0; i < subscribers; ++i) {
      const std::int64_t behind = final_ts - last_seen[i].load();
      if (behind > 0 &&
          static_cast<std::uint64_t>(behind) > r.worst_staleness_updates) {
        r.worst_staleness_updates = static_cast<std::uint64_t>(behind);
      }
    }
  }

  if (r.sync_publish_ms > 0.0) {
    r.sync_updates_per_sec =
        static_cast<double>(updates) * 1000.0 / r.sync_publish_ms;
  }
  if (r.conflating_publish_ms > 0.0) {
    r.conflating_updates_per_sec =
        static_cast<double>(updates) * 1000.0 / r.conflating_publish_ms;
    r.speedup = r.sync_publish_ms / r.conflating_publish_ms;
  }
  return r;
}

}  // namespace basis::bench

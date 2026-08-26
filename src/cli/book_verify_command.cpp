#include "cli/commands.h"

#include <cstdio>
#include <string>
#include <vector>

#include "cli/args.h"
#include "cli/usage.h"
#include "core/logger.h"
#include "feed/binance_parser.h"
#include "feed/book_sequencer.h"
#include "feed/feed_log.h"
#include "model/order_book.h"

#ifdef BASIS_HAS_BDE
#endif

namespace basis::cli {

// Rebuilding a book from a diff stream and checking it.
//
// analysis_commands.cpp held all four capture-reading subcommands and was
// the largest file in the repo at 737 lines. They share includes and
// nothing else: one generates a capture, one replays it, one measures
// which venue's price moves first, one reconciles a rebuilt book against
// the venue's own snapshot. Splitting on that seam is what the earlier
// main.cpp split did, and for the same reason.

int run_book_verify_cmd(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string path(args[0]);
  const auto depth = flag_value(args, "--levels", 20);
  if (!depth || *depth <= 0 || *depth > 1000) {
    basis::log::error("book-verify: bad --levels");
    return usage();
  }
  std::ifstream in(path);
  if (!in) {
    basis::log::error("book-verify: cannot open " + path);
    return 1;
  }

  simdjson::dom::parser parser;
  basis::feed::BookSequencer seq;
  // price cents -> size, mirroring the venue's own book.
  std::map<std::int64_t, std::string, std::greater<std::int64_t>> bids;
  std::map<std::int64_t, std::string> asks;
  std::string validation;
  std::int64_t target = -1;
  std::int64_t final_u = -1;
  bool reached = false;

  const auto to_cents = [](std::string_view s) {
    return static_cast<std::int64_t>(std::llround(std::strtod(
        std::string(s).c_str(), nullptr) * 100.0));
  };
  // Every level is [price, qty]; a shorter array is malformed input, not
  // a reason to read past the end. binance_parser.cpp guards the same
  // shape and this had not carried the guard over.
  const auto load_side = [&](simdjson::dom::element levels, bool bid) -> bool {
    simdjson::dom::array arr;
    if (levels.get_array().get(arr) != simdjson::SUCCESS) return false;
    for (auto lv : arr) {
      simdjson::dom::array pair;
      if (lv.get_array().get(pair) != simdjson::SUCCESS) return false;
      auto it = pair.begin();
      if (it == pair.end()) return false;
      std::string_view px;
      if ((*it).get_string().get(px) != simdjson::SUCCESS) return false;
      ++it;
      if (it == pair.end()) return false;
      std::string_view qty;
      if ((*it).get_string().get(qty) != simdjson::SUCCESS) return false;
      const std::int64_t cents = to_cents(px);
      const bool empty = std::strtod(std::string(qty).c_str(), nullptr) == 0.0;
      if (bid) {
        if (empty) bids.erase(cents); else bids[cents] = std::string(qty);
      } else {
        if (empty) asks.erase(cents); else asks[cents] = std::string(qty);
      }
    }
    return true;
  };
  const auto fail = [](const char* why) {
    basis::log::error(std::string("book-verify: ") + why);
    return 1;
  };

  std::string line;
  while (std::getline(in, line)) {
    const auto t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const auto t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    const std::string kind = line.substr(t1 + 1, t2 - t1 - 1);
    const std::string payload = line.substr(t2 + 1);
    simdjson::dom::element doc;
    if (parser.parse(simdjson::padded_string(payload)).get(doc) !=
        simdjson::SUCCESS) {
      continue;
    }
    if (kind == "snapshot") {
      std::int64_t last = 0;
      if (doc["lastUpdateId"].get_int64().get(last) != simdjson::SUCCESS) {
        return fail("snapshot has no lastUpdateId");
      }
      bids.clear();
      asks.clear();
      if (!load_side(doc["bids"], true) || !load_side(doc["asks"], false)) {
        return fail("snapshot levels malformed");
      }
      seq.on_snapshot(last);
    } else if (kind == "validation_snapshot") {
      if (doc["lastUpdateId"].get_int64().get(target) != simdjson::SUCCESS) {
        return fail("validation snapshot has no lastUpdateId");
      }
      validation = payload;
    } else if (kind == "diff" && !reached) {
      std::int64_t U = 0, u = 0;
      if (doc["U"].get_int64().get(U) != simdjson::SUCCESS ||
          doc["u"].get_int64().get(u) != simdjson::SUCCESS) {
        return fail("depth event has no update-id range");
      }
      switch (seq.on_update(U, u)) {
        case basis::feed::BookSequencer::Decision::Apply:
          if (!load_side(doc["b"], true) || !load_side(doc["a"], false)) {
            return fail("depth event levels malformed");
          }
          if (target >= 0 && u >= target) {
            reached = true;
            final_u = u;
          }
          break;
        case basis::feed::BookSequencer::Decision::Gap:
          // A real feed would refetch here. In a verification run a gap
          // means the answer is "cannot verify", not a silent retry.
          break;
        default:
          break;
      }
    }
  }

  const auto& st = seq.stats();
  // final_u == target means the reconstruction stopped exactly on the
  // validation snapshot's sequence point and the comparison below is
  // exact. Depth events are atomic, so a snapshot taken strictly inside
  // an event's range leaves the book a few ids past it; the overshoot is
  // reported rather than hidden, because a clean result at overshoot > 0
  // is weaker evidence than one at overshoot == 0 and a reader cannot
  // tell them apart otherwise.
  const std::int64_t overshoot =
      (reached && target >= 0) ? final_u - target : -1;
  std::printf("BOOK_VERIFY applied=%llu discarded=%llu buffered=%llu "
              "gaps=%llu stale_snapshots=%llu reached_target=%d "
              "final_u=%lld target=%lld overshoot=%lld\n",
              u(st.applied), u(st.discarded), u(st.buffered), u(st.gaps),
              u(st.stale_snapshots), reached ? 1 : 0,
              static_cast<long long>(final_u),
              static_cast<long long>(target),
              static_cast<long long>(overshoot));
  if (validation.empty() || !reached) {
    basis::log::error("book-verify: never reached the validation point");
    return 1;
  }

  simdjson::dom::element vdoc;
  if (parser.parse(simdjson::padded_string(validation)).get(vdoc) !=
      simdjson::SUCCESS) {
    basis::log::error("book-verify: validation snapshot unparseable");
    return 1;
  }
  int mismatches = 0, compared = 0;
  const auto compare = [&](const char* key, bool bid) {
    int i = 0;
    auto mine_bid = bids.begin();
    auto mine_ask = asks.begin();
    for (auto lv : simdjson::dom::array(vdoc[key])) {
      if (i++ >= *depth) break;
      auto it = simdjson::dom::array(lv).begin();
      const std::int64_t vpx = to_cents((*it).get_string().value());
      ++it;
      const double vqty = std::strtod(
          std::string((*it).get_string().value()).c_str(), nullptr);
      std::int64_t mpx = 0;
      double mqty = 0.0;
      if (bid) {
        if (mine_bid == bids.end()) { ++mismatches; continue; }
        mpx = mine_bid->first;
        mqty = std::strtod(mine_bid->second.c_str(), nullptr);
        ++mine_bid;
      } else {
        if (mine_ask == asks.end()) { ++mismatches; continue; }
        mpx = mine_ask->first;
        mqty = std::strtod(mine_ask->second.c_str(), nullptr);
        ++mine_ask;
      }
      ++compared;
      if (mpx != vpx || std::fabs(mqty - vqty) > 1e-9) ++mismatches;
    }
  };
  compare("bids", true);
  compare("asks", false);
  std::printf("BOOK_VERIFY levels_compared=%d mismatches=%d %s\n",
              compared, mismatches, mismatches == 0 ? "exact" : "DIVERGED");
  return mismatches == 0 ? 0 : 1;
}
}  // namespace basis::cli

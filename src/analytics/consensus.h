#pragma once

#include "analytics/event_study.h"
#include "analytics/lead_lag.h"

namespace basis::analytics {

// Which venue a method says leads. A is Kalshi, B is Polymarket, matching the
// lead-lag convention (positive lead_seconds = A leads) and the event study's
// forward direction (A's moves answered by B).
enum class Leader { None, A, B };

// The payoff of running two unrelated methods: do the cross-correlation and
// the event study agree on which venue leads? PLAN.md's methodology rests on
// this -- either method alone can be an artifact of its own assumptions, but
// two independent methods pointing the same way is the defensible finding.
struct LeadConsensus {
  Leader crosscorr = Leader::None;    // significant direction from lead-lag
  Leader event_study = Leader::None;  // confirmed direction from the study

  // Both methods resolved a direction and it is the same one.
  bool agree() const {
    return crosscorr != Leader::None && crosscorr == event_study;
  }
  // Both resolved a direction but disagree on the leader -- a red flag that
  // the apparent lead is method-dependent, not real.
  bool conflict() const {
    return crosscorr != Leader::None && event_study != Leader::None &&
           crosscorr != event_study;
  }
  // The agreed leader, or None unless both methods agree.
  Leader leader() const { return agree() ? crosscorr : Leader::None; }
};

// Combine a single event's two estimates into a consensus. The cross-
// correlation resolves a direction only when its bootstrap interval excludes
// zero; the event study only when its follow-rate z clears the bar. Either
// staying None means that method could not tell -- not that it disagreed.
inline LeadConsensus lead_consensus(const LeadLagResult& ll,
                                    const EventStudyResult& es) {
  LeadConsensus out;
  if (ll.lead_is_significant()) {
    out.crosscorr = ll.lead_seconds > 0.0 ? Leader::A : Leader::B;
  }
  switch (es.confirmed_leader()) {
    case 1:  out.event_study = Leader::A; break;
    case -1: out.event_study = Leader::B; break;
    default: out.event_study = Leader::None; break;
  }
  return out;
}

}  // namespace basis::analytics

#ifndef UDP_BRIDGE_DROP_BASELINE_H
#define UDP_BRIDGE_DROP_BASELINE_H

#include <atomic>
#include <cstdint>

namespace udp_bridge
{

/// Monotonic baseline for a "dropped since the last tick" delta (issue #51).
///
/// The queue diagnostics report the increase in a cumulative drop counter
/// since the previous tick, rather than latching a WARN forever after one
/// historical drop. That needs a stored baseline — and the baseline has two
/// writers:
///
///   * `diagnosePublishQueue` / `diagnoseRelayQueue`, on the diagnostic
///     timer, which read the total, subtract the baseline and store the
///     total back; and
///   * `on_deactivate`, which re-baselines to the post-stop total so the
///     backlog the operator just discarded is not reported as link loss.
///     The lifecycle transition callback does not run in `periodic_group_`,
///     so it can interleave with a tick.
///
/// Interleaved, a plain `uint64_t` gives both a data race and a wrong
/// answer: the tick reads total = 100, `on_deactivate` stores 120, the tick
/// subtracts 120 from 100 and underflows to ~2^64 — the discarded backlog
/// reported as an enormous burst of link loss, which is precisely the
/// misreport the re-baselining was added to prevent.
///
/// This advances the baseline to `total` and returns the value it had
/// before, never moving it backwards. Monotonic because both writers only
/// ever publish the same non-decreasing cumulative total: whichever wins,
/// the other's delta is computed against a baseline that is at least as
/// new, so a drop is reported at most once and never negatively.
///
/// `relaxed` ordering is enough — the value carries no other state, and the
/// counters it is compared against are independently atomic.
inline uint64_t advanceDropBaseline(std::atomic<uint64_t>& baseline,
                                    uint64_t total)
{
  uint64_t previous = baseline.load(std::memory_order_relaxed);
  while(previous < total &&
        !baseline.compare_exchange_weak(previous, total,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed))
  {
    // compare_exchange_weak refreshed `previous` with the current value.
  }
  return previous;
}

/// Drops to report this tick: the increase over the baseline, which
/// `advanceDropBaseline` has just advanced. Zero when the baseline already
/// covers `total` — i.e. when a concurrent re-baseline got there first.
/// Never underflows, which is the whole point.
inline uint64_t dropsSinceBaseline(uint64_t total, uint64_t previous)
{
  return total > previous ? total - previous : 0;
}

} // namespace udp_bridge

#endif

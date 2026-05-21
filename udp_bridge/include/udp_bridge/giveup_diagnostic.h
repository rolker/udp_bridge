#ifndef UDP_BRIDGE_GIVEUP_DIAGNOSTIC_H
#define UDP_BRIDGE_GIVEUP_DIAGNOSTIC_H

#include <cmath>
#include <cstdint>
#include <string>

#include "diagnostic_msgs/msg/diagnostic_status.hpp"
#include "rclcpp/time.hpp"

namespace udp_bridge
{

// Per-remote resend-give-up diagnostic snapshot. Computed by
// computeGiveupDiagnostic() from a counter pair + elapsed time, then
// surfaced by UDPBridge's diagnostic_updater task. Pure logic, no
// node / clock / mutex dependencies — the wrapper holds those.
struct GiveupDiagnostic
{
  // diagnostic_msgs::msg::DiagnosticStatus level value (OK / WARN / ERROR).
  uint8_t level;
  // Give-ups per second over the elapsed interval. Zero on first call,
  // zero on counter reset, zero on zero-elapsed.
  double rate_per_s;
  // Cumulative count from RemoteNode::resendGiveupCount() — surfaced
  // for operators who want the total alongside the rate.
  uint32_t total;
};

// Compute the diagnostic from a counter pair + elapsed window.
//
// Edge cases (tested in test_giveup_diagnostic.cpp):
//  - First call:        prev_count == 0 && elapsed_s == 0
//                         → rate_per_s = 0, level = OK.
//  - Zero elapsed time: elapsed_s == 0 (or negative, e.g. clock jump)
//                         → rate_per_s = 0, level = OK. Never divides.
//  - Counter reset:     current_count < prev_count
//                         (per #21, the counter has been observed to
//                         rewind across a sender restart)
//                         → rate_per_s = 0, level = OK; total reflects
//                         current_count.
//  - No activity:       current_count == prev_count
//                         → rate_per_s = 0, level = OK regardless of
//                         thresholds. Threshold semantics apply to
//                         give-up activity, not its absence; without
//                         this short-circuit, a 0/0 threshold pair
//                         (allowed by validateGiveupThresholds) would
//                         fire ERROR on every quiet tick via `0 >= 0`.
//  - Threshold transitions (current_count > prev_count, elapsed_s > 0):
//      rate >= error_thresh    → level = ERROR
//      rate >= warn_thresh     → level = WARN
//      otherwise               → level = OK
//
// The function is intentionally a pure free function so the test can
// exercise it without a UDPBridge instance or a clock — the
// time-dependence is moved out into the caller. Defined inline because
// the body is small and pure; this avoids needing a separate .cpp +
// CMakeLists entry for ~10 lines of arithmetic.
inline GiveupDiagnostic computeGiveupDiagnostic(
    uint32_t prev_count,
    uint32_t current_count,
    double elapsed_s,
    double warn_thresh,
    double error_thresh)
{
  GiveupDiagnostic d;
  d.total = current_count;

  // Counter reset (sender restart per #21) and zero-or-negative elapsed
  // both collapse to "no measurable rate this interval". elapsed_s < 0
  // covers clock jumps from sim time or NTP corrections.
  if (current_count < prev_count || elapsed_s <= 0.0)
  {
    d.rate_per_s = 0.0;
    d.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
    return d;
  }

  // No activity this window: report OK regardless of thresholds. The
  // `>=` comparisons below would otherwise fire WARN/ERROR for a zero
  // rate when warn_thresh / error_thresh are 0 — a permanently-non-OK
  // diagnostic. Caught by Copilot round 3 on PR #24; regression test
  // ZeroDeltaWithZeroThresholdsStaysOk.
  if (current_count == prev_count)
  {
    d.rate_per_s = 0.0;
    d.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
    return d;
  }

  d.rate_per_s = static_cast<double>(current_count - prev_count) / elapsed_s;

  if (d.rate_per_s >= error_thresh)
    d.level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
  else if (d.rate_per_s >= warn_thresh)
    d.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
  else
    d.level = diagnostic_msgs::msg::DiagnosticStatus::OK;

  return d;
}

// Per-remote state for the rate-window calculation. The UDPBridge
// wrapper holds one instance per remote and snapshots/updates it via
// stepGiveupDiagnostic() under remote_nodes_mutex_. Carrying state in
// a struct (rather than two parallel maps) makes the map-update
// ordering testable without bringing up a UDPBridge node — see
// stepGiveupDiagnostic() below.
struct GiveupRateState
{
  uint32_t last_count {0};
  rclcpp::Time last_publish_time;
  // has_previous distinguishes "no prior call yet" from "prior call
  // observed count=0 at time=0", which would otherwise be ambiguous
  // for a freshly-started bridge whose clock hasn't yet reached
  // wall-time. Without this flag the first call would compute
  // elapsed_s = (now - default_time) ≈ now.seconds(), which fires a
  // spurious huge rate on the very first publish.
  bool has_previous {false};
};

// Stateful step: snapshots `state`, computes a diagnostic from the
// (prev, current, elapsed) tuple, and writes the new sample back
// before returning. Caller holds whatever lock guards `state`.
//
// First call (state.has_previous == false): treated as zero-elapsed
// (rate=0, level=OK). State is updated so the second call has a
// baseline. This intentionally suppresses a "first window" rate even
// if current_count > 0 — the wrapper has no way to know how long
// those give-ups accumulated over, so reporting a rate would be
// misleading.
inline GiveupDiagnostic stepGiveupDiagnostic(
    GiveupRateState& state,
    uint32_t current_count,
    rclcpp::Time now_time,
    double warn_thresh,
    double error_thresh)
{
  uint32_t prev_count = 0;
  double elapsed_s = 0.0;
  if(state.has_previous)
  {
    prev_count = state.last_count;
    elapsed_s = (now_time - state.last_publish_time).seconds();
  }
  state.last_count = current_count;
  state.last_publish_time = now_time;
  state.has_previous = true;
  return computeGiveupDiagnostic(
      prev_count, current_count, elapsed_s, warn_thresh, error_thresh);
}

// Result of validating a proposed (warn, error) threshold pair.
// `ok == true` means the pair is safe to apply; `reason` is empty.
// `ok == false` means the pair is invalid; `reason` carries an
// operator-facing explanation suitable for SetParametersResult.reason
// or an on_configure ERROR log.
struct GiveupThresholdValidation
{
  bool ok;
  std::string reason;
};

// Validate a proposed (warn, error) threshold pair before applying.
// Rejects:
//  - NaN / inf  — non-finite values silently disable the diagnostic
//                 because NaN comparisons always return false.
//  - Negative   — rate is always >= 0, so a negative threshold never
//                 fires; an operator-error signal, not a configuration.
//  - warn > err — inverts the WARN band: rates above warn would also
//                 be above error and fire ERROR, never WARN.
// The error string identifies which parameter is bad so an operator
// can correct it from `ros2 param set` feedback without digging
// through logs.
inline GiveupThresholdValidation validateGiveupThresholds(
    double warn, double error)
{
  if(!std::isfinite(warn))
    return {false, "resend_giveup_warn_rate_per_s must be finite; got "
                   + std::to_string(warn)};
  if(warn < 0.0)
    return {false, "resend_giveup_warn_rate_per_s must be >= 0; got "
                   + std::to_string(warn)};
  if(!std::isfinite(error))
    return {false, "resend_giveup_error_rate_per_s must be finite; got "
                   + std::to_string(error)};
  if(error < 0.0)
    return {false, "resend_giveup_error_rate_per_s must be >= 0; got "
                   + std::to_string(error)};
  if(warn > error)
    return {false, "resend_giveup_warn_rate_per_s (" + std::to_string(warn)
                   + ") must be <= resend_giveup_error_rate_per_s ("
                   + std::to_string(error) + ")"};
  return {true, ""};
}

}  // namespace udp_bridge

#endif  // UDP_BRIDGE_GIVEUP_DIAGNOSTIC_H

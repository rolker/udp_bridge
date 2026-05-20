#ifndef UDP_BRIDGE_GIVEUP_DIAGNOSTIC_H
#define UDP_BRIDGE_GIVEUP_DIAGNOSTIC_H

#include <cstdint>

#include "diagnostic_msgs/msg/diagnostic_status.hpp"

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
//  - Threshold transitions:
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

  d.rate_per_s = static_cast<double>(current_count - prev_count) / elapsed_s;

  if (d.rate_per_s >= error_thresh)
    d.level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
  else if (d.rate_per_s >= warn_thresh)
    d.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
  else
    d.level = diagnostic_msgs::msg::DiagnosticStatus::OK;

  return d;
}

}  // namespace udp_bridge

#endif  // UDP_BRIDGE_GIVEUP_DIAGNOSTIC_H

#ifndef UDP_BRIDGE_CONNECTION_DIAGNOSTIC_H
#define UDP_BRIDGE_CONNECTION_DIAGNOSTIC_H

#include <cstdint>
#include <string>

#include "diagnostic_msgs/msg/diagnostic_status.hpp"

namespace udp_bridge
{

/// Level + summary line for a per-connection DiagnosticStatus.
struct ConnectionDiagnostic
{
  /// diagnostic_msgs::msg::DiagnosticStatus level value.
  uint8_t level;
  /// The summary an operator reads first.
  std::string message;
};

/// Compute the per-connection diagnostic level and summary (#52).
///
/// Pure logic, extracted from UDPBridge::diagnoseConnection for the same
/// reason computeGiveupDiagnostic was: the wrapper owns the node, the
/// clock and the mutexes, and none of them are needed to decide what the
/// operator should be told.
///
/// The distinction this exists to draw is between a socket-level
/// FAILURE and deliberate admission SHEDDING. They shared one wording
/// ("tx failures/drops") through the 2026-08-25 incident, and a
/// throttled connection sits in that state continuously — so the one
/// message an operator had told them nothing about which of two
/// unrelated faults, with two unrelated remedies, they were looking at.
///
/// Precedence, highest first:
///   1. rx older than `stale_error_s`     -> ERROR (the link is down)
///   2. any tx failure                    -> WARN  (socket-level)
///   3. any tx drop                       -> WARN  (admission shedding,
///                                          naming the cap in force)
///   4. rx older than `stale_warn_s`      -> WARN
///   5. otherwise                         -> OK
///
/// A cap below the configured limit is named in the OK summary too: a
/// throttled link that is otherwise healthy is exactly the state that
/// reads as "slow for no reason", which is the whole complaint behind
/// this issue.
///
/// @param rx_age_s seconds since the last receive, or negative when
///        nothing has ever been received (an idle link is not a stale
///        one).
inline ConnectionDiagnostic computeConnectionDiagnostic(
    uint32_t configured_rate_limit,
    uint32_t effective_rate_limit,
    double tx_ok_bytes_per_sec,
    double tx_failed_bytes_per_sec,
    double tx_dropped_bytes_per_sec,
    double rx_bytes_per_sec,
    double rx_age_s,
    double stale_warn_s,
    double stale_error_s)
{
  using DS = diagnostic_msgs::msg::DiagnosticStatus;

  const bool throttled = effective_rate_limit < configured_rate_limit;
  const std::string cap_phrase = std::to_string(effective_rate_limit) + " of "
                               + std::to_string(configured_rate_limit) + " B/s";

  if(rx_age_s >= 0.0 && rx_age_s > stale_error_s)
    return {DS::ERROR,
            "no rx for " + std::to_string(static_cast<uint64_t>(rx_age_s)) + "s"};

  if(tx_failed_bytes_per_sec > 0.0)
    return {DS::WARN, "tx failures"};

  if(tx_dropped_bytes_per_sec > 0.0)
    return {DS::WARN,
            throttled ? "shedding at admission cap " + cap_phrase
                      : "shedding at configured rate limit "
                          + std::to_string(configured_rate_limit) + " B/s"};

  if(rx_age_s >= 0.0 && rx_age_s > stale_warn_s)
    return {DS::WARN,
            "no rx for " + std::to_string(static_cast<uint64_t>(rx_age_s)) + "s"};

  std::string summary = "tx " + std::to_string(static_cast<uint64_t>(tx_ok_bytes_per_sec))
                      + " B/s, rx " + std::to_string(static_cast<uint64_t>(rx_bytes_per_sec))
                      + " B/s";
  if(throttled)
    summary += ", admission cap " + cap_phrase;
  return {DS::OK, summary};
}

}  // namespace udp_bridge

#endif

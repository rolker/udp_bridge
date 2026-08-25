#ifndef UDP_BRIDGE_CONNECTION_TUNABLES_H
#define UDP_BRIDGE_CONNECTION_TUNABLES_H

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "udp_bridge/resend_constants.h"

namespace udp_bridge
{

// Runtime-settable per-connection tunables.
//
// Every parameter below is declared in on_configure and read there once.
// Until this header existed, that was the ONLY time they were read: a
// `ros2 param set` of, say, `admission_floor_bytes_per_second` was
// accepted by rclcpp, read back with the new value, and changed nothing
// in the live Connection. That cost a wrong conclusion and a ~3 minute
// link outage on the 2026-08-25 BizzyBoat transit, where the operator
// raised the floor to escape an admission-control collapse and watched
// the collapse continue.
//
// Two pieces live here so they can be unit-tested without a node (the
// same isolation-by-extraction pattern as giveup_diagnostic.h):
//
//   - TunableTarget: which live object a parameter name drives. The map
//     from name to target is built in on_configure, where the parameter
//     names are constructed in the first place, rather than parsed back
//     out of the name at set time. Parsing would be wrong: a remote's
//     wire name may itself contain '.' (see the #67 note in
//     udp_bridge/README.md), so `remotes.<r>.connections.<c>.<key>` is
//     not unambiguously splittable on '.'.
//   - validate*(): the value checks the OnSetParameters callback runs
//     before applying anything.
//
// Validation policy, and why it differs from on_configure's:
//
//   on_configure CLAMPS an out-of-range value (or falls back to the
//   default) and carries on. That behaviour is kept: a YAML file has no
//   return channel, and refusing to start a telemetry bridge over a
//   mistyped scalar is worse than starting it with a sane value.
//
//   A runtime set DOES have a return channel — `ros2 param set` prints
//   the SetParametersResult reason — so an out-of-range value is
//   rejected with a message naming the parameter and the accepted
//   range. Rejecting also keeps the parameter store and the live object
//   in agreement: an accepted value is one the setter stores unchanged,
//   so a subsequent `ros2 param get` reports what is actually in force.
//   (The one deliberate exception is a connection's
//   `maximum_bytes_per_second` of 0, whose documented meaning is "use
//   Connection::default_rate_limit"; see below.)

/// Result of validating one proposed runtime parameter value. `ok ==
/// true` means it is safe to apply and `reason` is empty; `ok == false`
/// means it must be refused and `reason` is the operator-facing
/// explanation for SetParametersResult::reason.
struct TunableValidation
{
  bool ok;
  std::string reason;
};

/// Which live setting a runtime-settable parameter drives.
enum class TunableKind
{
  /// Connection::setAdmissionFloorBytesPerSecond
  AdmissionFloorBytesPerSecond,
  /// Connection::setLinkHeadroomFraction
  LinkHeadroomFraction,
  /// Connection::setResendBudgetFraction
  ResendBudgetFraction,
  /// Connection::setRateLimit
  ConnectionMaximumBytesPerSecond,
};

/// Where a runtime-settable parameter applies. Built in on_configure
/// alongside the declaration of the parameter it is keyed by, so the
/// strings here are exactly the ones the rest of the node keys on:
/// `remote` is the remotes_list entry (which since #67 is also the
/// remote's wire name and the routing-table key), `connection_id` is
/// the connection name as configured (RemoteNode::connection()
/// canonicalizes it on lookup).
struct TunableTarget
{
  TunableKind kind;
  std::string remote;
  std::string connection_id;
};

/// Largest double that survives the cast to the float fields Connection
/// stores these in. Beyond it the cast yields inf, which would read back
/// from the getter as a different value than the parameter store holds.
inline constexpr double kMaxTunableFloat =
  static_cast<double>(std::numeric_limits<float>::max());

/// `admission_floor_bytes_per_second`: absolute floor of the
/// AIMD-adjusted admission cap. Must be finite and non-negative.
///
/// There is deliberately no upper bound beyond float representability:
/// the floor is clamped AT USE to the connection's own
/// maximum_bytes_per_second (Connection::updateAdmissionControl), so a
/// floor above the cap yields the cap rather than raising it, and a
/// connection whose limit is raised later does not carry a silently
/// truncated floor. Negative is refused rather than clamped for the
/// reason the setter documents: 0 disables the floor entirely, so
/// "degrade to the nearest bound" would degrade to "off" — the one
/// outcome the floor exists to prevent.
inline TunableValidation validateAdmissionFloorBytesPerSecond(
    const std::string& name, double value)
{
  if(!std::isfinite(value))
    return {false, name + " must be finite; got " + std::to_string(value)};
  if(value < 0.0)
    return {false, name + " must be >= 0; got " + std::to_string(value)};
  if(value > kMaxTunableFloat)
    return {false, name + " must be <= " + std::to_string(kMaxTunableFloat)
                   + "; got " + std::to_string(value)};
  return {true, ""};
}

/// `link_headroom_fraction`: share of measured goodput left unused for
/// co-tenant traffic. Must be finite and within [0, kMaxLinkHeadroomFraction].
///
/// The upper bound is the setter's clamp, not 1.0: a headroom of exactly
/// 1.0 would target zero throughput on every congested sample and the
/// connection could never carry data again. Refusing anything the setter
/// would clamp keeps `ros2 param get` honest about what is in force.
inline TunableValidation validateLinkHeadroomFraction(
    const std::string& name, double value)
{
  if(!std::isfinite(value))
    return {false, name + " must be finite; got " + std::to_string(value)};
  if(value < 0.0 || value > static_cast<double>(kMaxLinkHeadroomFraction))
    return {false, name + " must be in [0, "
                   + std::to_string(kMaxLinkHeadroomFraction) + "]; got "
                   + std::to_string(value)};
  return {true, ""};
}

/// `resend_budget_fraction`: share of measured goodput resends may
/// consume. Must be finite and within [0, 1] — the setter's clamp.
inline TunableValidation validateResendBudgetFraction(
    const std::string& name, double value)
{
  if(!std::isfinite(value))
    return {false, name + " must be finite; got " + std::to_string(value)};
  if(value < 0.0 || value > 1.0)
    return {false, name + " must be in [0, 1]; got " + std::to_string(value)};
  return {true, ""};
}

/// A connection's `maximum_bytes_per_second`. ROS 2 integer parameters
/// are int64 and Connection stores a uint32_t, so a negative or
/// over-range value is refused rather than wrapped. Zero is accepted
/// and keeps its established meaning: use Connection::default_rate_limit.
inline TunableValidation validateMaximumBytesPerSecond(
    const std::string& name, int64_t value)
{
  if(value < 0)
    return {false, name + " must be >= 0; got " + std::to_string(value)};
  if(value > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
    return {false, name + " must be <= "
                   + std::to_string(std::numeric_limits<uint32_t>::max())
                   + "; got " + std::to_string(value)};
  return {true, ""};
}

}  // namespace udp_bridge

#endif  // UDP_BRIDGE_CONNECTION_TUNABLES_H

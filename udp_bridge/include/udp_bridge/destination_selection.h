#ifndef UDP_BRIDGE_DESTINATION_SELECTION_H
#define UDP_BRIDGE_DESTINATION_SELECTION_H

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "rclcpp/time.hpp"
#include "rclcpp/duration.hpp"

#include "udp_bridge/types.h"

namespace udp_bridge
{

/// Per-remote list of connection ids to send over. Mirrors
/// UDPBridge::RemoteConnectionsList (kept as a plain typedef here so this
/// header has no dependency on UDPBridge and stays unit-testable).
using SelectedConnections = std::map<std::string, std::vector<std::string> >;

/// The per-destination fields a sender copies out of RemoteDetails while
/// holding subscribers_mutex_, so the (slow) send loop doesn't need to
/// re-lock. Shared by the local-origin and relay senders.
struct DestinationConfig
{
  std::string destination_topic;
  std::string reliability;
  std::string durability;
  uint32_t history_depth = 0;
};

/// True when some remote other than `source_node` appears in this topic's
/// routing table — i.e. when a message received from `source_node` on this
/// topic has somewhere to be relayed (issue #51).
///
/// This is the same loop rule selectRateLimitedConnections applies, in a
/// read-only form: it is the cheap probe the socket-drain thread runs before
/// paying for a payload copy, so the two must agree. A symmetric two-host
/// pair (each side listing only the other) always answers false — which is
/// why relay changes nothing for every configuration in this repo.
///
/// An **empty** `source_node` answers false, always. The loop rule is a
/// comparison against the sender's name, so an unnamed sender cannot be
/// excluded from anything: relaying such a packet would send it to every
/// remote listing the topic, the sender included. `node_name` is empty for
/// any packet that did not pass through `unwrap()` — a bare, unwrapped Data
/// packet, which on an unauthenticated transport (#53) anyone who can reach
/// the port can inject. Refusing to relay it is the conservative answer:
/// the packet is still published locally, exactly as before #51.
inline bool hasRelayDestination(
  const std::map<std::string, RemoteDetails>& remote_details,
  const std::string& source_node)
{
  if(source_node.empty())
    return false;
  for(const auto& remote: remote_details)
    if(remote.first != source_node)
      return true;
  return false;
}

/// Choose which (remote, connection) pairs are due to receive a message
/// right now, and stamp their last_sent_time.
///
/// This is the single implementation of the bridge's per-connection rate
/// limiting, shared by BOTH senders (issue #51):
///
///   - UDPBridge::callback(), for a message published locally, and
///   - UDPBridge::relayToOtherRemotes(), for a message that arrived from
///     another remote and is being forwarded on.
///
/// Sharing it is the point: relayed traffic is throttled by exactly the
/// same `period` rule, against exactly the same last_sent_time state, as
/// locally-originated traffic. A parallel implementation would drift.
///
/// Behaviour (unchanged from the pre-#51 inline block in callback()):
///
///   - A negative period means "never send" and is skipped.
///   - period == 0 means "no rate limit".
///   - Otherwise a connection is due when it has never been sent to
///     (last_sent_time == 0) or `now - last_sent_time > period`.
///   - `periods` groups connections that share a period: once a connection
///     with period P has been selected in this call, every LATER connection
///     (in `connection_rates` iteration order, i.e. by connection id) with
///     the same P is selected too, so same-rate connections fan out
///     together rather than skewing between them.
///
///     The grouping is deliberately described as order-dependent, because
///     it is: a not-yet-due connection encountered *before* the first due
///     one with the same period is skipped, and only the connections after
///     it join the group. Pre-#51 behaviour, preserved verbatim — noted
///     here so the asymmetry is not mistaken for a guarantee.
///   - Every selected connection's last_sent_time is set to `now`.
///
/// @param remote_details the routing table for one topic
///        (SubscriberDetails::remote_details). Mutated: last_sent_time is
///        updated for each selected connection, so the caller must hold
///        whatever mutex guards it (UDPBridge: subscribers_mutex_).
/// @param now current time
/// @param exclude_remote if non-empty, the remote of this name is skipped
///        entirely — no connection of it is selected and, critically, none
///        of its last_sent_time values is touched, so an excluded remote's
///        rate-limit state is exactly as if this call had not happened.
///        This is the relay loop rule: a message is never sent back to the
///        remote it came from. Empty for the local-origin path, which has
///        no remote to exclude.
inline SelectedConnections selectRateLimitedConnections(
  std::map<std::string, RemoteDetails>& remote_details,
  const rclcpp::Time& now,
  const std::string& exclude_remote = std::string())
{
  SelectedConnections destinations;
  for(auto& remote: remote_details)
  {
    // Loop rule (#51): never send back to the sender. Applied before any
    // last_sent_time is touched, so exclusion leaves no side effect.
    if(!exclude_remote.empty() && remote.first == exclude_remote)
      continue;
    std::unordered_set<float> periods; // group the sending to connections with same period
    for(auto& connection_rate: remote.second.connection_rates)
      if(connection_rate.second.period >= 0)
        if(connection_rate.second.period == 0 || connection_rate.second.last_sent_time.nanoseconds() == 0 || now-connection_rate.second.last_sent_time > rclcpp::Duration::from_seconds(connection_rate.second.period) || periods.count(connection_rate.second.period) > 0)
        {
          destinations[remote.first].push_back(connection_rate.first);
          connection_rate.second.last_sent_time = now;
          if(periods.count(connection_rate.second.period) == 0)
            periods.insert(connection_rate.second.period);
        }
  }
  return destinations;
}

} // namespace udp_bridge

#endif

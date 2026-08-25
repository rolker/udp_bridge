#ifndef UDP_BRIDGE_DESTINATION_SELECTION_H
#define UDP_BRIDGE_DESTINATION_SELECTION_H

#include <algorithm>
#include <cstdint>
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
/// This is the read-only form of what selectRateLimitedConnections would
/// decide: it is the cheap probe the socket-drain thread runs before paying
/// for a payload copy, so the two must agree. A symmetric two-host pair
/// (each side listing only the other) always answers false — which is why
/// relay changes nothing for every configuration in this repo.
///
/// It applies both of the selector's structural filters: the loop rule (the
/// sender is skipped) and `period < 0`, which means "never send" — a remote
/// whose every connection is set to a negative period can never be
/// selected, so answering true for it would cost a payload copy, a relay
/// queue slot and a worker wakeup for a message that is then dropped at
/// selection. What the probe deliberately does NOT apply is the *temporal*
/// part of the rate limit (`now - last_sent_time`): that depends on the
/// clock and mutates `last_sent_time`, so it belongs to the selector alone,
/// on the relay worker. The probe is therefore may-send, never must-send.
///
/// The per-topic byte cap is temporal in exactly the same way (it depends
/// on the clock and mutates the token bucket), so the probe does not apply
/// it either. A topic sitting at its cap therefore still pays for the copy
/// and the queue slot before being shed at selection — the cost of keeping
/// the probe a pure, side-effect-free read.
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
  {
    if(remote.first == source_node)
      continue;
    for(const auto& connection_rate: remote.second.connection_rates)
      if(connection_rate.second.period >= 0)
        return true;
  }
  return false;
}

/// What one call to selectRateLimitedConnections decided.
///
/// `due` is where the message goes. `over_budget` is the (remote,
/// connection) pairs that WERE due under `period` but exceeded their own
/// per-topic `maximum_bytes_per_second` — they are dropped, and the
/// caller records them as dropped so the shedding is visible in the
/// topic's statistics instead of being silent. Both are always
/// populated; a caller that ignores `over_budget` turns a rate limit
/// into invisible loss, which is why this is a struct rather than an
/// optional out-parameter.
struct RateLimitedSelection
{
  SelectedConnections due;
  SelectedConnections over_budget;
};

/// Charge `message_bytes` against one connection's per-topic byte
/// budget. Returns true when the message is admitted (and charges it),
/// false when it is over budget (and charges nothing).
///
/// A token bucket, not a tumbling window: the accumulator drains
/// continuously at the cap, and the bucket depth is one second of the
/// cap. That gives the same instantaneous allowance as the strict
/// 1-second sliding window PacketSendStatistics::can_send applies at the
/// connection level, without keeping a per-topic deque of timestamps in
/// a map that forwarding callbacks mutate on the hot path.
///
/// Three edge cases are deliberate:
///
///  - **Cap 0 is unlimited**, so an absent `maximum_bytes_per_second`
///    key leaves every existing configuration behaving exactly as before.
///  - **An empty bucket admits any single message, however large.**
///    Without this a message bigger than one second of the cap (a camera
///    frame against a modest cap) could never be admitted at all — the
///    topic would go permanently silent instead of slow. Admitting it
///    leaves the bucket overdrawn, and the drain then holds the topic
///    off for exactly as long as the overdraft implies, so the sustained
///    rate is still the cap. This mirrors the resend budget's probe
///    guarantee (Connection::resend_packets), which exists for the same
///    reason.
///  - **A backwards clock step resets the bucket** rather than
///    computing a negative drain. Sim time and NTP corrections both
///    produce one; the conservative direction here is to forget the
///    charge, since the alternative is a bucket that can never drain.
inline bool admitAgainstTopicByteCap(ConnectionRateInfo& rate,
                                     uint64_t message_bytes,
                                     const rclcpp::Time& now)
{
  if(rate.maximum_bytes_per_second == 0)
    return true;

  const double cap = static_cast<double>(rate.maximum_bytes_per_second);
  if(!rate.budget_started)
  {
    // First charge on this connection. Note that budget_updated_time is
    // still default-constructed here (RCL_SYSTEM_TIME), so it must not
    // be subtracted from `now`.
    rate.budget_used_bytes = 0.0;
  }
  else
  {
    const double elapsed = (now - rate.budget_updated_time).seconds();
    if(elapsed < 0.0)
      rate.budget_used_bytes = 0.0;
    else
      rate.budget_used_bytes =
        std::max(0.0, rate.budget_used_bytes - cap * elapsed);
  }
  rate.budget_updated_time = now;
  rate.budget_started = true;

  if(rate.budget_used_bytes > 0.0 &&
     rate.budget_used_bytes + static_cast<double>(message_bytes) > cap)
    return false;

  rate.budget_used_bytes += static_cast<double>(message_bytes);
  return true;
}

/// Build the statistics record for a set of over-budget destinations, so
/// a per-topic drop lands in TopicStatistics' `send` DataRates exactly as
/// a connection-level drop does.
///
/// `sent_size` is the message payload size. The message was never
/// serialized, so its wire size does not exist — the payload size is the
/// honest stand-in and is the same quantity the cap is expressed in. The
/// per-destination rows of TopicStatistics therefore mix payload bytes
/// (shed here) with wire bytes (shed by Connection::send) in
/// `dropped_bytes_per_second`; that is a known and documented seam, and
/// the alternative — recording the drop with no size — would hide the
/// shedding altogether.
inline MessageSizeData overBudgetDropRecord(
  const SelectedConnections& over_budget,
  std::size_t message_bytes,
  const rclcpp::Time& now)
{
  MessageSizeData data;
  data.timestamp = now;
  data.message_size = static_cast<int>(message_bytes);
  data.sent_size = static_cast<int>(message_bytes);
  for(const auto& remote: over_budget)
    for(const auto& connection_id: remote.second)
      data.send_results[remote.first][connection_id] = SendResult::dropped;
  return data;
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
///   - A connection that is due under `period` is then charged
///     `message_bytes` against its own per-topic
///     `maximum_bytes_per_second` (0 = unlimited, the default). Over
///     budget, it is reported in `over_budget` instead of `due`, and
///     neither its last_sent_time nor the same-period grouping is
///     touched — nothing was sent to it, so nothing about its send state
///     should move. Charging happens here, in the one place both senders
///     already meter, so relayed traffic is capped by exactly the same
///     rule and the same state as locally published traffic.
///
/// @param remote_details the routing table for one topic
///        (SubscriberDetails::remote_details). Mutated: last_sent_time and
///        the per-topic byte budget are updated for each selected
///        connection, so the caller must hold whatever mutex guards it
///        (UDPBridge: subscribers_mutex_). The mutation is bounded
///        arithmetic on values already in the map — no I/O — so it fits
///        inside the brief critical section both senders already take,
///        and nothing here is held across a sendto.
/// @param now current time
/// @param message_bytes size of the message being offered, in payload
///        bytes, for the per-topic byte cap. Required rather than
///        defaulted: a caller that forgets it would silently disable
///        every per-topic cap, which is the failure mode this whole
///        mechanism exists to make impossible.
/// @param exclude_remote if non-empty, the remote of this name is skipped
///        entirely — no connection of it is selected and, critically, none
///        of its last_sent_time values is touched, so an excluded remote's
///        rate-limit state is exactly as if this call had not happened.
///        This is the relay loop rule: a message is never sent back to the
///        remote it came from. Empty for the local-origin path, which has
///        no remote to exclude.
inline RateLimitedSelection selectRateLimitedConnections(
  std::map<std::string, RemoteDetails>& remote_details,
  const rclcpp::Time& now,
  uint64_t message_bytes,
  const std::string& exclude_remote = std::string())
{
  RateLimitedSelection selection;
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
          // Due under `period`. The per-topic byte cap is the second
          // gate, applied only to connections that got this far: a
          // connection the period rule already held back must not have
          // its budget charged for a message it was never going to
          // receive.
          if(!admitAgainstTopicByteCap(connection_rate.second, message_bytes, now))
          {
            selection.over_budget[remote.first].push_back(connection_rate.first);
            continue;
          }
          selection.due[remote.first].push_back(connection_rate.first);
          connection_rate.second.last_sent_time = now;
          if(periods.count(connection_rate.second.period) == 0)
            periods.insert(connection_rate.second.period);
        }
  }
  return selection;
}

} // namespace udp_bridge

#endif

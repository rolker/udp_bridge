#ifndef UDP_BRIDGE_TYPES_H
#define UDP_BRIDGE_TYPES_H

#include <cstdint>

#include "udp_bridge/statistics.h"
#include "rclcpp/generic_subscription.hpp"

namespace udp_bridge
{

struct ConnectionRateInfo
{
  /// Minimum delay between messages, seconds. 0 = no limit, negative =
  /// never send. Default-initialised: every writer sets it explicitly,
  /// but a map entry created by operator[] is readable (by
  /// hasRelayDestination, among others) before the assignment lands, and
  /// an indeterminate float there is undefined behaviour.
  float period = 0.0f;
  rclcpp::Time last_sent_time;

  /// When this connection's `period` was last SATISFIED, which is not
  /// the same thing as when it was last sent to.
  ///
  /// A connection selected by the period rule and then shed by its own
  /// per-topic byte cap has consumed its slot in the period cohort: the
  /// message reached it, the cap is what stopped it, and the next one
  /// is not due until `period` has elapsed. Without a stamp of its own
  /// it stayed due on EVERY message (`last_sent_time` deliberately does
  /// not move for a shed connection — nothing was sent), so it re-opened
  /// its period group every time and pulled every later-iterating
  /// same-period sibling through `periods.count() > 0`, past that
  /// sibling's own period check. An uncapped sibling then sent at the
  /// full publish rate on the very link the cap was meant to protect
  /// (#75).
  ///
  /// Stamped on both outcomes — sent and shed — so it reads as "the
  /// period rule was last answered here". The due test in
  /// selectRateLimitedConnections requires the period to have elapsed
  /// against BOTH stamps, so a caller that only knows about
  /// `last_sent_time` keeps its existing behaviour.
  rclcpp::Time period_satisfied_time;

  /// Optional per-topic send cap for this (topic, remote, connection),
  /// in bytes per second of OFFERED MESSAGE PAYLOAD. 0 (the default,
  /// and what an absent `maximum_bytes_per_second` key configures)
  /// means unlimited — today's behaviour, so no existing config
  /// changes.
  ///
  /// This is a per-topic floor-guard for the connection's other topics:
  /// the connection-level cap is metered first-come-first-served in
  /// Connection::send, with no notion of which topic a packet belongs
  /// to, so one topic can take the whole link. Measured on BizzyBoat
  /// 2026-08-25: four camera streams and a bulk topic each delivering
  /// about a third of their bytes at the same time, with nothing able
  /// to prefer one over another.
  ///
  /// Units are message payload bytes, NOT the wire bytes the
  /// connection-level `maximum_bytes_per_second` meters. The decision
  /// happens before serialization, compression and fragmentation, so a
  /// wire figure does not exist yet; the payload figure is also what
  /// TopicStatistics::message_bytes_per_second reports, which is the
  /// number an operator sizes this against. See
  /// admitAgainstTopicByteCap in destination_selection.h.
  uint32_t maximum_bytes_per_second = 0;

  /// Token-bucket state for maximum_bytes_per_second: bytes charged and
  /// not yet drained, and when they were last drained. Bucket depth is
  /// one second of the cap, matching the strict 1-second window
  /// PacketSendStatistics::can_send uses at the connection level.
  ///
  /// `budget_started` distinguishes "never charged" from "charged at
  /// time zero". It is load-bearing, not tidiness: a default-constructed
  /// rclcpp::Time carries RCL_SYSTEM_TIME, and subtracting it from a
  /// RCL_ROS_TIME `now` throws. The same hazard is why the period check
  /// below short-circuits on last_sent_time.nanoseconds() == 0.
  double budget_used_bytes = 0.0;
  rclcpp::Time budget_updated_time;
  bool budget_started = false;
};

struct RemoteDetails
{
  RemoteDetails()=default;
  //RemoteDetails(std::string const &destination_topic, float period):destination_topic(destination_topic),period(period)
  //{}

  std::string destination_topic;
  std::map<std::string, ConnectionRateInfo> connection_rates;

  // Per-topic QoS that the receiver should apply to the destination
  // publisher. See udp_bridge/doc/qos_design.md. Empty strings and 0
  // mean "use the package default". The current operational default is
  // RELIABLE / VOLATILE / KEEP_LAST(1) — RELIABLE is a temporary
  // override for the rmw_zenoh_cpp 0.2.9 graph-visibility issue
  // documented in qos_resolution.h; design intent is BEST_AVAILABLE.
  std::string reliability;
  std::string durability;
  uint32_t history_depth = 0;
};

struct SubscriberDetails
{
  rclcpp::GenericSubscription::SharedPtr subscription;
  uint32_t queue_size;
  std::map<std::string, RemoteDetails> remote_details;
  MessageStatistics statistics;

  // Source-side subscription QoS (the bridge's local subscriber). Set from
  // per-topic config. Reliability stays best_effort by convention (accepts
  // any source publisher); only durability is configurable, so the bridge
  // can subscribe transient_local when latching semantics matter.
  std::string durability;
};

// Name and address/port from which a message was received
struct SourceInfo
{
  std::string node_name;
  std::string host;
  uint16_t port = 0;

  // Sequencing metadata, populated by UDPBridge::unwrap from the
  // SequencedPacketHeader before the inner packet is recursively
  // decoded. `sequenced` is false for any packet that did not pass
  // through the wrapped-packet layer (so the stale-packet gate in
  // decodeData leaves un-sequenced packets untouched). `packet_number`
  // is the sender's per-remote-node monotonic sequence number, used to
  // drop late-arriving resends that a newer message has already
  // superseded on the same destination topic.
  bool sequenced = false;
  uint64_t packet_number = 0;
};

} // namespace udp_bridge

#endif

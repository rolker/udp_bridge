#ifndef UDP_BRIDGE_RELAY_ITEM_H
#define UDP_BRIDGE_RELAY_ITEM_H

#include <cstddef>
#include <string>

#include "udp_bridge_interfaces/msg/message_internal.hpp"

#include "udp_bridge/destination_selection.h"

namespace udp_bridge
{

/// One relay job handed from the socket-drain thread to the relay worker
/// (issue #51). It carries the message exactly as it arrived — the same
/// serialized MessageInternal that decodeData deserialized, so forwarding
/// costs no re-serialization of the payload — plus the immediate sender's
/// node name, which is the loop rule's only input: a relay is never sent
/// back to the remote it came from.
///
/// The handoff exists for the same reason PublishItem does (issue #10): the
/// socket-drain thread must never make a call that can block. Forwarding
/// runs Connection::send(), whose sendto() has a ~200 ms worst-case
/// MSG_DONTWAIT retry loop per stalled destination (connection.cpp), so N
/// stalled destinations would stack ~200 ms x N ahead of the next recvfrom.
struct RelayItem
{
  /// Destination topic as resolved by decodeData (destination_topic, or
  /// source_topic when that is empty). This is the key into subscribers_,
  /// i.e. the routing-table lookup for who else wants this traffic.
  std::string topic;

  /// Immediate sender (SourceInfo::node_name). The loop rule skips any
  /// destination whose remote name equals this — direct-neighbour only;
  /// see doc/relay_design.md for why cycles are unsupported.
  std::string source_node;

  /// The received message, forwarded byte-for-byte apart from the
  /// per-destination destination_topic / QoS fields the relay path
  /// overwrites for each recipient.
  udp_bridge_interfaces::msg::MessageInternal message;

  /// Approximate footprint for the queue's byte budget. The payload
  /// dominates; the small string fields are included so an empty-payload
  /// message still counts a nonzero amount.
  size_t byte_size() const
  {
    return message.data.size()
      + topic.size() + source_node.size()
      + message.source_topic.size() + message.destination_topic.size()
      + message.datatype.size();
  }
};

/// Rewrite the per-destination fields of a message being relayed, for one
/// recipient (issue #51).
///
/// `hub_topic` is the topic as THIS bridge knows it — the key into
/// `subscribers_`, i.e. the destination topic the upstream remote's
/// configuration mapped its own topic onto.
///
/// `source_topic` is rewritten to `hub_topic`, and that matters: the
/// receiving bridge resolves a message to `destination_topic`, falling back
/// to `source_topic` when it is empty (`decodeData`), and an empty
/// `destination_topic` is ordinary — `remoteAdvertise` /
/// `decodeSubscribeRequest` pass it through with no fallback of their own.
/// Left unrewritten, the fallback would land the message on the *upstream
/// publisher's* topic name instead of the hub-local one: two boats each
/// publishing `/nav`, remapped by the hub to `/boat_a/nav` and
/// `/boat_b/nav`, would collide back onto `/nav` downstream. Rewriting it
/// also makes a relayed message identical in shape to a locally-originated
/// one, whose `source_topic` is likewise the sending bridge's own topic
/// (`callback()`). Provenance is not lost: the immediate sender travels in
/// the wrapped packet's `source_node`, which is where the receiver reads it
/// from anyway.
inline void applyRelayDestination(
  udp_bridge_interfaces::msg::MessageInternal& message,
  const std::string& hub_topic,
  const DestinationConfig& config)
{
  message.source_topic      = hub_topic;
  message.destination_topic = config.destination_topic;
  message.reliability       = config.reliability;
  message.durability        = config.durability;
  message.history_depth     = config.history_depth;
}

} // namespace udp_bridge

#endif

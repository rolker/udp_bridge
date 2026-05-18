#ifndef UDP_BRIDGE_REMOTE_NODE_H
#define UDP_BRIDGE_REMOTE_NODE_H

#include "udp_bridge_interfaces/msg/remote.hpp"
#include "udp_bridge_interfaces/msg/resend_request.hpp"
#include "udp_bridge/packet.h"
#include "udp_bridge/defragmenter.h"
#include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge/types.h"

#include <mutex>
#include <set>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/node_interfaces/node_interfaces.hpp"

namespace udp_bridge
{

class Connection;

// Represents a remote udp_bridge node.
// Multiple connections to the remote node
// may be specified.
class RemoteNode
{
  public:

  using NodeInterfaces = rclcpp::node_interfaces::NodeInterfaces<
    rclcpp::node_interfaces::NodeTopicsInterface,
    rclcpp::node_interfaces::NodeLoggingInterface,
    rclcpp::node_interfaces::NodeClockInterface,
    rclcpp::node_interfaces::NodeBaseInterface
  >;
  

  RemoteNode(std::string remote_name, std::string local_name, NodeInterfaces node);

  void update(const udp_bridge_interfaces::msg::Remote& remote_message);
  void update(const udp_bridge_interfaces::msg::BridgeInfo& bridge_info, const SourceInfo& source_info);

  const std::string &name() const;

  // returns a name sutible for use as a topic name.
  std::string topicName() const;

  std::shared_ptr<Connection> connection(std::string connection_id);
  std::vector<std::shared_ptr<Connection> > connections();

  std::shared_ptr<Connection> newConnection(std::string connection_id, std::string host, uint16_t port);

  // Insert an externally-constructed Connection (e.g. one created on the
  // outbound CONNECT path before the remote acknowledged) into this
  // RemoteNode's connections_ map. No-op if a connection with the same
  // id is already registered.
  void adoptConnection(std::shared_ptr<Connection> connection);

  std::vector<uint8_t> unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  std::vector<std::vector<uint8_t> > getPacketsToResend(const udp_bridge_interfaces::msg::ResendRequest& resend_request);

  Defragmenter& defragmenter();

  void publishTopicStatistics(const udp_bridge_interfaces::msg::TopicStatisticsArray& statistics);

  void clearReceivedPacketTimesBefore(rclcpp::Time time);

  udp_bridge_interfaces::msg::ResendRequest getMissingPackets();

  // Time-injection overload used by unit tests so the gap-scan and
  // backoff logic can be exercised against a controlled clock without
  // bringing up a sim-time-driven node. Production callers should use
  // the no-arg variant above.
  udp_bridge_interfaces::msg::ResendRequest getMissingPackets(rclcpp::Time now);

  // Test helper: record a received packet at a caller-supplied time
  // without going through unwrap(). Used by the gtest suite to seed
  // received_packet_times_ at known times for the debounce / backoff /
  // give-up scenarios.
  void recordReceivedPacketTimeForTest(uint64_t packet_number, rclcpp::Time time);

  // Count of missing-packet resends this RemoteNode has given up on
  // because the sender's TTL expired before the packet arrived. Read
  // by UDPBridge::sendBridgeInfo() to populate
  // Remote.msg.resend_giveup_count. Lock-friendly with
  // sendBridgeInfo's scoped_lock(subscribers_, remote_nodes_): this
  // getter takes only the recursive state_mutex_ and does NOT
  // re-acquire remote_nodes_mutex_.
  uint32_t resendGiveupCount() const;

private:
  // name of the remote udp_bridge node
  std::string name_;

  // name of the local udp_bridge node;
  std::string local_name_;

  // Guards all mutable state below: connections_, received_packet_times_,
  // resend_request_times_, next_packet_number_, last_packet_time_. Under
  // MultiThreadedExecutor, public methods on RemoteNode are reachable from
  // multiple callback groups (socket-drain via decode/unwrap; periodic via
  // bridge_info / stats / diagnostic timers; service handlers in periodic
  // group).
  //
  // recursive_mutex chosen because some public methods call each other —
  // update(BridgeInfo) calls connection() and newConnection(); unwrap()
  // calls connection() and newConnection(); getMissingPackets() calls
  // clearReceivedPacketTimesBefore(). recursive_mutex avoids the
  // boilerplate of inner non-locking helpers.
  //
  // defragmenter_ is intentionally NOT guarded by this mutex: it is
  // accessed via defragmenter() which returns a reference. Concurrency
  // contract: defragmenter_ is only touched from socket-drain callbacks
  // (UDPBridge::decode for Fragment packets and the spin_once tail
  // cleanup loop), which are mutually-exclusive within socket_drain_group_
  // by construction. If a future caller reaches defragmenter() from a
  // different group, that contract breaks; document and lock then.
  mutable std::recursive_mutex state_mutex_;

  std::map<std::string, std::shared_ptr<Connection> > connections_;

  Defragmenter defragmenter_;

  std::map<uint64_t, rclcpp::Time> received_packet_times_;

  // Per-missing-packet state used by getMissingPackets to apply
  // exponential backoff and the TTL-bounded give-up condition (issue
  // #9 failure modes B and the give-up gap). Entries are created when
  // a packet is first observed missing; cleared by
  // clearReceivedPacketTimesBefore when first_request_time falls
  // outside kReceiveHistoryWindow OR by update(BridgeInfo) on remote
  // restart.
  struct ResendState
  {
    rclcpp::Time first_request_time;
    rclcpp::Time last_request_time;
    uint32_t attempts = 0;
  };
  std::map<uint64_t, ResendState> resend_state_;

  // Packet numbers we have already given up on (sender TTL expired
  // before the packet arrived). Prevents the backoff loop's
  // resend_state_[m] access — which uses operator[] and would
  // default-construct a fresh entry — from re-requesting a packet we
  // already gave up on. Entries are added by the give-up sweep at the
  // top of getMissingPackets and pruned by
  // clearReceivedPacketTimesBefore when the packet number falls below
  // received_packet_times_.begin()->first (no longer in the gap-scan
  // range, so no future getMissingPackets call can re-flag it). Also
  // cleared by update(BridgeInfo)'s remote-restart detection, since
  // the remote's packet number space resets.
  std::set<uint64_t> given_up_packet_numbers_;

  // Per-remote count of resend give-ups (sender TTL expired before the
  // packet arrived). Bumped in getMissingPackets when a missing
  // packet's first-request time is older than kSentPacketTTL. Exposed
  // via resendGiveupCount(). Cumulative since RemoteNode construction;
  // survives remote-restart detection (the counter is operator-facing
  // and meant to be read as "loss across this RemoteNode's lifetime").
  // ~225 days at 220 give-ups/s before uint32 wraparound; not a
  // realistic concern for any deployment.
  uint32_t resend_giveup_count_ = 0;

  rclcpp::Publisher<udp_bridge_interfaces::msg::BridgeInfo>::SharedPtr bridge_info_publisher_;
  rclcpp::Publisher<udp_bridge_interfaces::msg::TopicStatisticsArray>::SharedPtr topic_statistics_publisher_;

  uint64_t next_packet_number_ = 0;
  rclcpp::Time last_packet_time_;

  rclcpp::Logger logger_;
  rclcpp::Clock::SharedPtr clock_;
};

}  // namespace udp_bridge

#endif

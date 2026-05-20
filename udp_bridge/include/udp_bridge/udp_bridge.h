#ifndef UDP_BRIDGE_UDP_BRIDGE_H
#define UDP_BRIDGE_UDP_BRIDGE_H

// C system headers
#include <netinet/in.h>

// C++ standard library
#include <atomic>
#include <mutex>
#include <set>

// Other library / project includes
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "rclcpp/generic_publisher.hpp"
#include "rclcpp/generic_subscription.hpp"
#include "diagnostic_updater/diagnostic_updater.hpp"

#include "udp_bridge_interfaces/srv/subscribe.hpp"
#include "udp_bridge_interfaces/srv/add_remote.hpp"
#include "udp_bridge_interfaces/srv/list_remotes.hpp"
#include "udp_bridge_interfaces/msg/connection_internal.hpp"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"

#include "connection.h"
#include "giveup_diagnostic.h"
#include "packet.h"
#include "defragmenter.h"
#include "udp_bridge/types.h"
#include "udp_bridge/wrapped_packet.h"
//#include "std_msgs/msg/int32.hpp"

namespace udp_bridge
{

class RemoteNode;

/// The top level class containing all the functionality of the udp_bridge_node.
/// Once the constructor has read initial parameters and created the network socket,
/// the \ref spin method then sets up the services and reads the initial remote 
/// connections and topics from the parameter server before going into a loop to read 
/// incoming UDP packets and decode them.
///
/// Meanwhile, the \ref callback method handles sending ROS messages from locally 
/// subscribed topics to remote nodes.
///
/// Additional timer callbacks are used to locally publish and send to remotes
/// information about the local bridge (udp_bridge::UDPBridge::bridgeInfoCallback) and
/// data statistics (udp_bridge::UDPBridge::statsReportCallback).
class UDPBridge: public rclcpp_lifecycle::LifecycleNode
{
public:
  using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

  UDPBridge(const std::string &node_name = "udp_bridge");


  CallbackReturn on_configure(const rclcpp_lifecycle::State &);

  CallbackReturn on_activate(const rclcpp_lifecycle::State & state);

  CallbackReturn on_deactivate(const rclcpp_lifecycle::State & state);

  CallbackReturn on_cleanup(const rclcpp_lifecycle::State &);

  CallbackReturn on_shutdown(const rclcpp_lifecycle::State & state);

  /// Listens in a loop for incoming UDP packets and decodes them.
  /// The decode(std::vector<uint8_t> const &message, const SourceInfo& source_info) is
  /// called when a packet is received. 
  void spin_once();
    
private:
  /// Sets the node name as seen by other udp_bridge nodes
  /// Warns if truncated to size specified in packet header.
  void setName(const std::string &name);

  /// Callback method for locally subscribed topics.
  /// ShapeShifter is used to be agnostic of message type at compile time.
  void callback(std::string topic_name, std::string topic_type, std::shared_ptr<rclcpp::SerializedMessage> message);


  /// Decodes outer layer of packets received over the UDP link and calls appropriate
  /// handlers based on packet type.
  /// @param message packet data as vector of bytes
  /// @param source_info info about the packet sender
  ///
  /// The main packet sorting method. More specific packet decoding routines get 
  /// selected based on Packet::type.
  void decode(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  template <typename MessageType>
  MessageType deserialize(std::vector<uint8_t> const &message)
  {
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    auto data_size = message.size()-sizeof(PacketHeader);
    rclcpp::SerializedMessage serialized_message;
    serialized_message.reserve(data_size);
    memcpy(serialized_message.get_rcl_serialized_message().buffer, packet->data, data_size);
    serialized_message.get_rcl_serialized_message().buffer_length = data_size;
    MessageType deserialized_message;
    rclcpp::Serialization<MessageType> serializer;
    serializer.deserialize_message(&serialized_message, &deserialized_message);
    return deserialized_message;
  }
    
  /// Decodes data from a remote subscription received over the UDP link.
  /// @param message bytes representing a serialized MessageInternal message
  /// @param source_info info about the packet sender
  void decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info);
    
  /// Decodes topic info from remote.
  void decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes statistics from remote.
  void decodeTopicStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes a request from a remote node to subscribe to a local topic.
  void decodeSubscribeRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes a request from a remote node to resend packets.
  void decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Unwraps and decodes a packet.
  void unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes connection management messages.
  void decodeConnection(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Service handler for local request to subscribe to a remote topic.
  void remoteSubscribe(
    const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);

  /// Service handler to advertise on a remote node a local topic.
  void remoteAdvertise(
    const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);

  /// Service handler to add a named remote
  void addRemote(
    const std::shared_ptr<udp_bridge_interfaces::srv::AddRemote::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::AddRemote::Response> response);

  /// Service handler to list named remotes
  void listRemotes(
    const std::shared_ptr<udp_bridge_interfaces::srv::ListRemotes::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::ListRemotes::Response> response);


  /// Map of remotes and connections
  using RemoteConnectionsList = std::map<std::string, std::vector<std::string> >;

  /// Convert a message to a packet and send it to remotes.
  template <typename MessageType>
  MessageSizeData send(MessageType const &message, const RemoteConnectionsList& remotes, bool is_overhead);

  /// Convert a message to a packet and send it to remote using all connections.
  template <typename MessageType>
  MessageSizeData send(MessageType const &message, const std::string& remote, bool is_overhead);

  /// Return a list of all remotes.
  RemoteConnectionsList allRemotes() const;

  /// Timer callback where data rate stats are reported
  void statsReportCallback();

  /// Periodic tick: sync per-connection diagnostic tasks with current remotes
  /// and force the diagnostic_updater to publish.
  void diagnosticTick();

  /// Register a diagnostic task for any (remote, connection) pair not yet tracked.
  void syncDiagnosticTasks();

  /// Populate a diagnostic status message for one (remote, connection) pair.
  void diagnoseConnection(const std::string& remote_name,
                          const std::string& connection_id,
                          diagnostic_updater::DiagnosticStatusWrapper& stat);

  /// Populate a diagnostic status message summarizing resend give-up
  /// activity for one remote. Computes rate from a window since the
  /// previous publish (tracked in last_giveup_*_ maps guarded by
  /// remote_nodes_mutex_). The rate-vs-threshold logic lives in the
  /// computeGiveupDiagnostic() free function (giveup_diagnostic.h) so
  /// it can be unit-tested without a UDPBridge instance.
  void diagnoseRemoteGiveups(const std::string& remote_name,
                             diagnostic_updater::DiagnosticStatusWrapper& stat);

  /// Timer callback where info on available topics are periodically reported
  void bridgeInfoCallback();


  /// Send topics and remotes info to remotes and publishes locally
  void sendBridgeInfo();

  /// Send pending connection requests
  void sendConnectionRequests();

  /// Splits up a packet, if necessary.
  /// Returns a vector of fragment packets, or a vector with the data if fragmenting is not necessary.
  std::vector< std::vector<uint8_t> > fragment(const std::vector<uint8_t>& data);

  /// Remove old buffered packets
  void cleanupSentPackets();

  /// Find missing packet and request resend
  void resendMissingPackets();

  /// @brief Adds a local subscription.
  /// @param source_topic Local message topic
  /// @param destination_topic Message topic to publish at remote
  /// @param queue_size local queue size
  /// @param period Minimum delay between messages sent in seconds
  /// @param remote_node destination udp_bridge
  /// @param connection_id  connection to remote to use
  /// @param reliability per-topic destination publisher reliability
  ///                    ("reliable" default — see qos_resolution.h header
  ///                    comment for the rmw_zenoh_cpp workaround; design
  ///                    intent is "best_available". Also accepts
  ///                    "best_effort" and explicit "best_available".)
  /// @param durability per-topic durability ("volatile" default, "transient_local")
  /// @param history_depth KEEP_LAST(N); 0 means default 1
  void addSubscriberConnection(std::string const &source_topic,
                               std::string const &destination_topic,
                               uint32_t queue_size, float period,
                               std::string remote_node,
                               std::string connection_id,
                               std::string reliability = "",
                               std::string durability = "",
                               uint32_t history_depth = 0);

  /// @brief Checks configured local topics and attempts to subscribe
  void updateLocalSubscriptions();  



  //void maximumPacketSizeCallback(const std_msgs::Int32::ConstPtr& msg);

  /// Name used to identify this node to other udp_bridge nodes
  std::string name_;

  // socket_, port_, max_packet_size_ are set once in on_configure and
  // are read-only afterward — no synchronization needed.
  int socket_;
  uint16_t port_ {4200};
  int max_packet_size_ {65500};

  // Protocol counters — incremented from multiple callback groups via
  // send<>(), so atomic is required. fetch_add(1) gives a unique value
  // per call.
  std::atomic<uint32_t> next_fragmented_packet_id_ {0};

  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr subscribe_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr advertise_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::AddRemote>::SharedPtr add_remote_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::ListRemotes>::SharedPtr list_remotes_service_;

  rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::TopicStatisticsArray>::SharedPtr topic_statistics_publisher_;
  rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::BridgeInfo>::SharedPtr bridge_info_publisher_;

  //rclcpp::Subscriber  maximum_packet_size_subscriber_;

  /// Callback groups for the MultiThreadedExecutor.
  /// Invariants documented in udp_bridge_node.cpp.
  rclcpp::CallbackGroup::SharedPtr socket_drain_group_;
  rclcpp::CallbackGroup::SharedPtr republish_group_;
  rclcpp::CallbackGroup::SharedPtr periodic_group_;

  std::map<std::string, SubscriberDetails> subscribers_;
  mutable std::mutex subscribers_mutex_;

  std::map<std::string, rclcpp::GenericPublisher::SharedPtr> publishers_;
  mutable std::mutex publishers_mutex_;

  rclcpp::TimerBase::SharedPtr stats_report_timer_;
  rclcpp::TimerBase::SharedPtr bridge_info_timer_;
  rclcpp::TimerBase::SharedPtr spin_timer_;
  rclcpp::TimerBase::SharedPtr subscription_update_timer_;
  rclcpp::TimerBase::SharedPtr diagnostic_timer_;

  std::unique_ptr<diagnostic_updater::Updater> diagnostic_updater_;
  /// Names of diagnostic tasks already registered, so we don't double-add as
  /// remotes/connections appear.
  std::set<std::string> diagnostic_task_names_;

  // Protocol counters touched from multiple callback groups; atomic is
  // required to prevent duplicate packet numbers from corrupting the
  // resend protocol. last_packet_number_assign_time_ is stored as
  // nanoseconds so it can be a primitive atomic; readers reconstruct
  // an rclcpp::Time on access. The two writes (next_packet_number_++
  // and the time stamp) aren't atomic together — diagnostic skew
  // between bi.next_packet_number and bi.last_packet_time is acceptable
  // (both are diagnostic display).
  std::atomic<uint64_t> next_packet_number_ {0};
  std::atomic<int64_t> last_packet_number_assign_time_ns_ {0};

  std::atomic<uint64_t> next_connection_internal_message_sequence_number_ {0};

  struct PendingConnection
  {
    udp_bridge_interfaces::msg::ConnectionInternal message;
    std::shared_ptr<Connection> connection;
  };

  /// Map pending remote connections to their message sequence_number.
  std::map<uint64_t, PendingConnection> pending_connections_;
  mutable std::mutex pending_connections_mutex_;

  std::map<std::string, std::shared_ptr<RemoteNode> > remote_nodes_;
  mutable std::mutex remote_nodes_mutex_;

  // Per-remote diagnostic state for the resend-give-up rate
  // computation (issue #22). Guarded by remote_nodes_mutex_ —
  // extended scope, not a new mutex; entries are small
  // integer/timestamp structs and the diagnostic callback's read is
  // brief. Cleared in on_cleanup alongside diagnostic_task_names_ so
  // an activate→deactivate→activate cycle starts from a known state.
  // Consolidating the (count, time, has_previous) tuple into one
  // struct (vs. two parallel maps) makes the wrapper's update logic
  // testable via the stepGiveupDiagnostic helper in
  // giveup_diagnostic.h — see test_giveup_diagnostic.cpp's
  // wrapper-side cases.
  std::map<std::string, GiveupRateState> giveup_rate_state_;

  // Thresholds for the resend-give-up DiagnosticStatus level. Declared
  // as ROS 2 parameters in on_configure so deployments can override
  // without rebuild; defaults calibrated against the 2026-05-19
  // BizzyBoat storm (~3.9/s steady, ~700/s burst). Live-updated via
  // an OnSetParameters callback (handle below), so operators can tune
  // mid-storm without restarting the bridge. Reads/writes are guarded
  // by remote_nodes_mutex_ — diagnoseRemoteGiveups holds the lock when
  // it samples these values, the callback holds the lock when it
  // updates them.
  double resend_giveup_warn_rate_per_s_ {5.0};
  double resend_giveup_error_rate_per_s_ {50.0};

  // Handle for the parameter-change callback. Reset in on_cleanup so
  // re-configure cycles don't accumulate stale handles.
  rclcpp::Node::OnSetParametersCallbackHandle::SharedPtr
    on_set_parameters_handle_;
};

} // namespace udp_bridge

#endif

#ifndef UDP_BRIDGE_UDP_BRIDGE_H
#define UDP_BRIDGE_UDP_BRIDGE_H

#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "rclcpp/generic_publisher.hpp"
#include "rclcpp/generic_subscription.hpp"

#include "udp_bridge_interfaces/srv/subscribe.hpp"
#include "udp_bridge_interfaces/srv/add_remote.hpp"
#include "udp_bridge_interfaces/srv/list_remotes.hpp"

#include <netinet/in.h>
#include "connection.h"
#include "packet.h"
#include "defragmenter.h"
#include "udp_bridge/types.h"
#include "udp_bridge/wrapped_packet.h"
#include "udp_bridge_interfaces/msg/connection_internal.hpp"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"
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
  template <typename MessageType> MessageSizeData send(MessageType const &message, const RemoteConnectionsList& remotes, bool is_overhead);

  /// Convert a message to a packet and send it to remote using all connections.
  template <typename MessageType> MessageSizeData send(MessageType const &message, const std::string& remote, bool is_overhead);

  /// Return a list of all remotes.
  RemoteConnectionsList allRemotes() const;

  // /// Sends the raw data to the connection. Returns number of bytes sent.
  // int send(const std::vector<uint8_t>& data, const sockaddr_in* address);

  /// Timer callback where data rate stats are reported
  void statsReportCallback();

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

  void addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::string remote_node, std::string connection_id);

  //void maximumPacketSizeCallback(const std_msgs::Int32::ConstPtr& msg);

  /// Name used to identify this node to other udp_bridge nodes
  std::string name_;

  int m_socket;
  uint16_t m_port {4200};
  int m_max_packet_size {65500};
  uint32_t next_fragmented_packet_id_ {0};

  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr subscribe_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr advertise_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::AddRemote>::SharedPtr add_remote_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::ListRemotes>::SharedPtr list_remotes_service_;

  rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::TopicStatisticsArray>::SharedPtr topic_statistics_publisher_;
  rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::BridgeInfo>::SharedPtr bridge_info_publisher_;

  //rclcpp::Subscriber  maximum_packet_size_subscriber_;
    
  std::map<std::string, SubscriberDetails> m_subscribers;

  std::map<std::string, rclcpp::GenericPublisher::SharedPtr> m_publishers;

  rclcpp::TimerBase::SharedPtr stats_report_timer_;
  rclcpp::TimerBase::SharedPtr bridge_info_timer_;
  rclcpp::TimerBase::SharedPtr spin_timer_;

  uint64_t next_packet_number_ = 0;
  rclcpp::Time last_packet_number_assign_time_;

  uint64_t next_connection_internal_message_sequence_number_ = 0;

  struct PendingConnection
  {
    udp_bridge_interfaces::msg::ConnectionInternal message;
    std::shared_ptr<Connection> connection;
  };

  /// Map pending remote connections to their message sequence_number.
  std::map<uint64_t, PendingConnection> pending_connections_;

  std::map<std::string, std::shared_ptr<RemoteNode> > remote_nodes_;
};

} // namespace udp_bridge

#endif

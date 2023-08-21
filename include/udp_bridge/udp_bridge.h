#ifndef UDP_BRIDGE_UDP_BRIDGE_H
#define UDP_BRIDGE_UDP_BRIDGE_H

#include <topic_tools/shape_shifter.h>
#include "udp_bridge/Subscribe.h"
#include "udp_bridge/AddRemote.h"
#include "udp_bridge/ListRemotes.h"
#include <netinet/in.h>
#include "connection.h"
#include "packet.h"
#include "defragmenter.h"
#include "udp_bridge/types.h"
#include "udp_bridge/wrapped_packet.h"
#include "udp_bridge/ConnectionInternal.h"

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
class UDPBridge
{
public:
  UDPBridge();

  /// Listens in a loop for incoming UDP packets and decodes them.
  /// The decode(std::vector<uint8_t> const &message, const SourceInfo& source_info) is
  /// called when a packet is received. 
  void spin();
    
private:
  /// Sets the node name as seen by other udp_bridge nodes
  /// Warns if truncated to size specified in packet header.
  void setName(const std::string &name);

  /// Callback method for locally subscribed topics.
  /// ShapeShifter is used to be agnostic of message type at compile time.
  void callback(const ros::MessageEvent<topic_tools::ShapeShifter>& event);

  /// Decodes outer layer of packets received over the UDP link and calls appropriate
  /// handlers based on packet type.
  /// @param message packet data as vector of bytes
  /// @param source_info info about the packet sender
  ///
  /// The main packet sorting method. More specific packet decoding routines get 
  /// selected based on Packet::type.
  void decode(std::vector<uint8_t> const &message, const SourceInfo& source_info);
    
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
  bool remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

  /// Service handler to advertise on a remote node a local topic.
  bool remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

  /// Service handler to add a named remote
  bool addRemote(udp_bridge::AddRemote::Request &request, udp_bridge::AddRemote::Response &response);

  /// Service handler to list named remotes
  bool listRemotes(udp_bridge::ListRemotes::Request &request, udp_bridge::ListRemotes::Response &response);


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
  void statsReportCallback(const ros::TimerEvent&);

  /// Timer callback where info on available topics are periodically reported
  void bridgeInfoCallback(const ros::TimerEvent&);


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

  /// Name used to identify this node to other udp_bridge nodes
  std::string name_;

  int m_socket;
  uint16_t m_port {4200};
  int m_max_packet_size {65500};
  uint32_t next_fragmented_packet_id_ {0};

  ros::NodeHandle m_nodeHandle;

  ros::ServiceServer subscribe_service_;
  ros::ServiceServer advertise_service_;
  ros::ServiceServer add_remote_service_;
  ros::ServiceServer list_remotes_service_;

  ros::Publisher m_topicStatisticsPublisher;
  ros::Publisher m_bridge_info_publisher;
    
  std::map<std::string, SubscriberDetails> m_subscribers;

  std::map<std::string, ros::Publisher> m_publishers;

  ros::Timer stats_report_timer_;
  ros::Timer bridge_info_timer_;

  uint64_t next_packet_number_ = 0;
  ros::Time last_packet_number_assign_time_;

  uint64_t next_connection_internal_message_sequence_number_ = 0;

  struct PendingConnection
  {
    ConnectionInternal message;
    std::shared_ptr<Connection> connection;
  };

  /// Map pending remote connections to their message sequence_number.
  std::map<uint64_t, PendingConnection> pending_connections_;

  std::map<std::string, std::shared_ptr<RemoteNode> > remote_nodes_;
};

} // namespace udp_bridge

#endif

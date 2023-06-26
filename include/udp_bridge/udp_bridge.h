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

namespace udp_bridge
{

class RemoteNode;

class UDPBridge
{
public:
  UDPBridge();

  void spin();
    
private:
  // Sets the node name as seen by other udp_bridge nodes
  // Warns if truncated to size specified in packet header.
  void setName(const std::string &name);

  /// Callback method for locally subscribed topics.
  /// ShapeShifter is used to be agnostic of message type at compile time.
  void callback(const topic_tools::ShapeShifter::ConstPtr& msg, const std::string &topic_name);



  /// Decodes outer layer of packets received over the UDP link and calls appropriate handlers base
  /// on packet type.
  void decode(std::vector<uint8_t> const &message, const SourceInfo& source_info);
    
  /// Decodes data from a remote subscription recieved over the UDP link.
  void decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info);
    
  /// Decodes metadata used to decode remote messages.
  void decodeChannelInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes topic info from remote.
  void decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes statistics from remote.
  void decodeChannelStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes a request from a remote node to subscribe to a local topic.
  void decodeSubscribeRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes a request from a remote node to resend packets.
  void decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Unwraps and decodes a packet.
  void unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info);



  /// Service handler for local request to subscribe to a remote topic.
  bool remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

  /// Service handler to advertise on a remote node a local topic.
  bool remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

  /// Service handler to add a named remote
  bool addRemote(udp_bridge::AddRemote::Request &request, udp_bridge::AddRemote::Response &response);

  /// Service handler to list named remotes
  bool listRemotes(udp_bridge::ListRemotes::Request &request, udp_bridge::ListRemotes::Response &response);



  using RemoteConnectionsList = std::map<std::string, std::vector<std::string> >;

  // Convert a message to a packet and send it to remotes.
  template <typename MessageType> SizeData send(MessageType const &message, const RemoteConnectionsList& remotes);

  // Convert a message to a packet and send it to remote using all connections.
  template <typename MessageType> SizeData send(MessageType const &message, const std::string& remote);

  // Return a list of all remotes.
  RemoteConnectionsList allRemotes() const;

  /// Sends the raw data to the connection. Returns number of bytes sent.
  int send(const std::vector<uint8_t>& data, const sockaddr_in* address);

  // /// Wraps the raw data and sends it to the connection. Returns true on success.
  // bool send(std::shared_ptr<std::vector<uint8_t> > data, const std::map<std::string, std::vector<std::string> >& remotes = {});

  /// Timer callback where data rate stats are reported
  void statsReportCallback(const ros::TimerEvent&);

  /// Timer callback where info on subscibed channels are periodically reported
  void bridgeInfoCallback(const ros::TimerEvent&);


  /// Send topics and remotes info to remotes and publishes locally
  void sendBridgeInfo();

  /// Splits up a packet, if necessary.
  /// Returns a vector of fragment packets, or a vector with the data if fragmenting is not necessary.
  std::vector< std::vector<uint8_t> > fragment(const std::vector<uint8_t>& data);

  /// Remove old buffered packets
  void cleanupSentPackets();

  /// Find missing packet and request resend
  void resendMissingPackets();

  void addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::string remote_node, std::string connection_id);

  // Name used to identify this node to other udp_bridge nodes
  std::string name_;

  int m_socket;
  uint16_t m_port {4200};
  int m_max_packet_size {65500};
  uint32_t next_fragmented_packet_id_ {0};

  ros::NodeHandle m_nodeHandle;
    
  ros::Publisher m_channelInfoPublisher;
  ros::Publisher m_bridge_info_publisher;

    
  std::map<std::string, SubscriberDetails> m_subscribers;

  std::map<std::string, ros::Publisher> m_publishers;
  std::map<std::string, ros::Time> m_channelInfoSentTimes;

  std::map<std::string, std::string> local_topic_types_;

  uint64_t next_packet_number_ = 0;
  ros::Time last_packet_number_assign_time_;

  Statistics overhead_statistics_;
  Statistics resend_statistics_;

  std::map<std::string, std::shared_ptr<RemoteNode> > remote_nodes_;
};

} // namespace udp_bridge

#endif

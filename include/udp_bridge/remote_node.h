#ifndef UDP_BRIDGE_REMOTE_NODE_H
#define UDP_BRIDGE_REMOTE_NODE_H

#include <udp_bridge/Remote.h>
#include <udp_bridge/ResendRequest.h>
#include <udp_bridge/packet.h>
#include <udp_bridge/defragmenter.h>
#include "udp_bridge/ChannelInfo.h"
#include <udp_bridge/ChannelStatisticsArray.h>
#include <udp_bridge/BridgeInfo.h>
#include <udp_bridge/types.h>

#include <ros/ros.h>

namespace udp_bridge
{

class Connection;

// Represents a remote udp_bridge node.
// Multiple connections to the remote node
// may be specified.
class RemoteNode
{
public:
  RemoteNode(std::string remote_name, std::string local_name);

  void update(const Remote& remote_message);
  void update(const BridgeInfo& bridge_info, const SourceInfo& source_info);

  const std::string &name() const;

  // returns a name sutible for use as a topic name.
  std::string topicName() const;

  std::shared_ptr<Connection> connection(std::string connection_id);
  std::vector<std::shared_ptr<Connection> > connections();

  std::shared_ptr<Connection> newConnection(std::string connection_id, std::string host, uint16_t port);

  std::vector<uint8_t> unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  std::vector<std::vector<uint8_t> > getPacketsToResend(const ResendRequest& resend_request);

  Defragmenter& defragmenter();

  std::map<std::string, ChannelInfo>& channelInfos();

  void publishChannelStatistics(const ChannelStatisticsArray& statistics);

  void clearReceivedPacketTimesBefore(ros::Time time);

  ResendRequest getMissingPackets();

private:
  // name of the remote udp_bridge node
  std::string name_;

  // name of the local udp_bridge node;
  std::string local_name_;

  std::map<std::string, std::shared_ptr<Connection> > connections_;

  Defragmenter defragmenter_;

  std::map<uint64_t, ros::Time> received_packet_times_;
  std::map<uint64_t, ros::Time> resend_request_times_;

  // map source topics to ChannelInfos
  std::map<std::string, ChannelInfo> channelInfos_;

  ros::Publisher bridge_info_publisher_;
  ros::Publisher channel_statistics_publisher_;

  uint64_t next_packet_number_ = 0;
  ros::Time last_packet_time_;
};

}  // namespace udp_bridge

#endif

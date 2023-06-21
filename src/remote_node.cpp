#include "udp_bridge/remote_node.h"
#include "udp_bridge/BridgeInfo.h"
#include "udp_bridge/connection.h"
#include "udp_bridge/utilities.h"

namespace udp_bridge
{

RemoteNode::RemoteNode(std::string remote_name, std::string local_name): name_(remote_name), local_name_(local_name)
{
  assert(remote_name != local_name);
  ros::NodeHandle private_nodeHandle("~");

  bridge_info_publisher_ = private_nodeHandle.advertise<BridgeInfo>("remotes/"+topicName()+"/bridge_info",1,true);

  channel_statistics_publisher_ = private_nodeHandle.advertise<ChannelStatisticsArray>("remotes/"+topicName()+"/channel_statistics",1,true);
}

void RemoteNode::update(const Remote& remote_message)
{
  assert(name_ == remote_message.name);
  for(auto c: remote_message.connections)
  {
    auto connection = connections_[c.connection_id];
    if(!connection)
      connection = newConnection(c.connection_id, c.host, c.port);
    connection->setReturnHostAndPort(c.return_host, c.return_port);
  }
}

void RemoteNode::update(const BridgeInfo& bridge_info, const SourceInfo& source_info)
{
  bridge_info_publisher_.publish(bridge_info);
  for(auto remote: bridge_info.remotes)
    if(remote.name == local_name_)
      for(auto connection_info: remote.connections)
      {
        auto c = connection(connection_info.connection_id);
        auto host = connection_info.return_host;
        if(host.empty())
          host = source_info.host;
        auto port = connection_info.return_port;
        if(port == 0)
          port = source_info.port;
        if(!c)
          c = newConnection(connection_info.connection_id, host, port);
        if(c)
        {
          c->setHostAndPort(host, port);
        }
      }
}


std::string RemoteNode::topicName() const
{
  return topicString(name_);
}

std::shared_ptr<Connection> RemoteNode::connection(std::string connection_id)
{
  if(connections_.find(connection_id) != connections_.end())
    return connections_[connection_id];
  return {};
}

std::vector<std::shared_ptr<Connection> > RemoteNode::connections()
{
  std::vector<std::shared_ptr<Connection> > ret;
  for(auto connection: connections_)
    if(connection.second)
      ret.push_back(connection.second);
  return ret;
}

std::shared_ptr<Connection> RemoteNode::newConnection(std::string connection_id, std::string host, uint16_t port)
{
  connections_[connection_id] = std::make_shared<Connection>(name_, connection_id, host, port);
  return connections_[connection_id];
}


std::vector<uint8_t> RemoteNode::unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  if(message.size() < sizeof(SequencedPacketHeader) || reinterpret_cast<const SequencedPacketHeader*>(message.data())->packet_size != message.size())
  {
    ROS_ERROR_STREAM("Can't unwrap packet of size " << message.size());
    if(message.size() >= sizeof(SequencedPacketHeader))
      ROS_ERROR_STREAM("Packet reports size: " << reinterpret_cast<const SequencedPacketHeader*>(message.data())->packet_size);
  }
  else
  {
    auto packet = reinterpret_cast<const SequencedPacket*>(message.data());
    if(received_packet_times_.find(packet->packet_number) != received_packet_times_.end())
      return {}; // skip duplicate packet
    received_packet_times_[packet->packet_number] = ros::Time::now();
    auto c = connection(packet->channel_id);
    if(!c)
      c = newConnection(packet->channel_id, source_info.host, source_info.port);
    c->update_last_receive_time(ros::Time::now().toSec(), message.size());
    try
    {
      return std::vector<uint8_t>(message.begin()+sizeof(SequencedPacketHeader), message.end());
    }
    catch(const std::exception& e)
    {
      ROS_ERROR_STREAM("problem decoding packet: " << e.what());
    }
  }  
  return {};
}

Defragmenter& RemoteNode::defragmenter()
{
  return defragmenter_;
}

std::map<std::string, ChannelInfo>& RemoteNode::channelInfos()
{
  return channelInfos_;
}

void RemoteNode::publishChannelStatistics(const ChannelStatisticsArray& statistics)
{
  channel_statistics_publisher_.publish(statistics);
}

void RemoteNode::clearReceivedPacketTimesBefore(ros::Time time)
{
  std::vector<uint64_t> expired;
  for(auto pt: received_packet_times_)
    if(pt.second < time)
      expired.push_back(pt.first);
  for(auto e: expired)
    received_packet_times_.erase(e);

  expired.clear();
  for(auto rt: resend_request_times_)
    if(rt.second < time)
      expired.push_back(rt.first);
  for(auto e: expired)
    resend_request_times_.erase(e);
}

std::map<uint64_t, ros::Time>& RemoteNode::receivedPacketTimes()
{
  return received_packet_times_;
}

std::map<uint64_t, ros::Time>& RemoteNode::resendRequestTimes()
{
  return resend_request_times_;
}


} // namespace udp_bridge

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

  topic_statistics_publisher_ = private_nodeHandle.advertise<TopicStatisticsArray>("remotes/"+topicName()+"/topic_statistics",1,true);
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
    connection->setRateLimit(c.maximum_bytes_per_second);
  }
}

void RemoteNode::update(const BridgeInfo& bridge_info, const SourceInfo& source_info)
{
  bridge_info_publisher_.publish(bridge_info);
  for(auto remote: bridge_info.remotes)
    if(remote.name == local_name_)
    {
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
        else
        {
          if(!connection_info.return_host.empty() || connection_info.return_port != 0)
            c->setHostAndPort(connection_info.return_host, connection_info.return_port);
        }
        c->setSourceIPAndPort(source_info.host, source_info.port);
      }
      if(bridge_info.next_packet_number < next_packet_number_)
      {
        ROS_WARN_STREAM("Received next packet number that is less than previous one: " << bridge_info.next_packet_number << " time: " << bridge_info.last_packet_time << " (previous: " << next_packet_number_ << " time: " << last_packet_time_ << ")");
        if(bridge_info.next_packet_number == 0 || bridge_info.last_packet_time > last_packet_time_)
        {
          // Try to detect remote restart of udp_bridge. Assume a reset if next packet number is zero
          // or if a timestamp for a lower numbered packet is greater than what we last saw for a larger
          // packet number.
          ROS_WARN_STREAM("Assuming remote udp_bridge restart");
          received_packet_times_.clear();
          resend_request_times_.clear();
        }
      } 
      next_packet_number_ = bridge_info.next_packet_number;
      last_packet_time_ = bridge_info.last_packet_time;
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
  connections_[connection_id] = std::make_shared<Connection>(connection_id, host, port);
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
    bool duplicate = received_packet_times_.find(packet->packet_number) != received_packet_times_.end();
    auto c = connection(packet->connection_id);
    if(!c)
      c = newConnection(packet->connection_id, source_info.host, source_info.port);
    c->update_last_receive_time(ros::Time::now().toSec(), message.size(), duplicate);
    if(!duplicate)
    {
      received_packet_times_[packet->packet_number] = ros::Time::now();
      try
      {
        return std::vector<uint8_t>(message.begin()+sizeof(SequencedPacketHeader), message.end());
      }
      catch(const std::exception& e)
      {
        ROS_ERROR_STREAM("problem decoding packet: " << e.what());
      }
    }
  }  
  return {};
}

Defragmenter& RemoteNode::defragmenter()
{
  return defragmenter_;
}

void RemoteNode::publishTopicStatistics(const TopicStatisticsArray& statistics)
{
  topic_statistics_publisher_.publish(statistics);
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

ResendRequest RemoteNode::getMissingPackets()
{
  ros::Time now = ros::Time::now();
  if(now.isValid() && !now.isZero())
  {
    ros::Time too_old = now - ros::Duration(5.0);
    clearReceivedPacketTimesBefore(too_old);
    ros::Time can_resend_time = now - ros::Duration(0.2);

    std::vector<uint64_t> missing;
    if(!received_packet_times_.empty())
      for(auto i = received_packet_times_.begin()->first+1; i < received_packet_times_.rbegin()->first; i++)
        if(received_packet_times_.find(i) == received_packet_times_.end())
          missing.push_back(i);
    ResendRequest rr;
    for(auto m: missing)
    {
      if(resend_request_times_[m] < can_resend_time)
      {
        rr.missing_packets.push_back(m);
        resend_request_times_[m] = now;
      }
    }
    return rr;
  }
  return {};
}

} // namespace udp_bridge

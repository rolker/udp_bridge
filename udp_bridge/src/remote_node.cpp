#include "udp_bridge/remote_node.h"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge/connection.h"
#include "udp_bridge/resend_constants.h"
#include "udp_bridge/utilities.h"
#include "rmw/qos_profiles.h"

namespace udp_bridge
{

using namespace udp_bridge_interfaces::msg;

RemoteNode::RemoteNode(std::string remote_name, std::string local_name, NodeInterfaces node): name_(remote_name), local_name_(local_name), logger_(node.get_node_logging_interface()->get_logger()),clock_(node.get_node_clock_interface()->get_clock())
{
  assert(remote_name != local_name);

  rclcpp::QoS latched_qos(1);
  latched_qos.transient_local();
  latched_qos.keep_last(1);

  std::string node_name = node.get_node_base_interface()->get_name();
  bridge_info_publisher_ = rclcpp::create_publisher<BridgeInfo>(node, node_name+"/remotes/"+topicName()+"/bridge_info", latched_qos);

  topic_statistics_publisher_ = rclcpp::create_publisher<TopicStatisticsArray>(node, node_name+"/remotes/"+topicName()+"/topic_statistics", latched_qos);
}

void RemoteNode::update(const Remote& remote_message)
{
  assert(name_ == remote_message.name);
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
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
  // Publish first, without the lock — bridge_info_publisher_ is set once
  // at construction and rclcpp publishers are thread-safe; holding our
  // mutex across publish() would violate the "no slow operations under
  // lock" discipline.
  bridge_info_publisher_->publish(bridge_info);

  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
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
        RCLCPP_WARN_STREAM(logger_, "Received next packet number that is less than previous one: " << bridge_info.next_packet_number << " time: " << rclcpp::Time(bridge_info.last_packet_time).seconds() << " (previous: " << next_packet_number_ << " time: " << rclcpp::Time(last_packet_time_).seconds() << ")");
        if(bridge_info.next_packet_number == 0 || rclcpp::Time(bridge_info.last_packet_time) > last_packet_time_)
        {
          // Try to detect remote restart of udp_bridge. Assume a reset if next packet number is zero
          // or if a timestamp for a lower numbered packet is greater than what we last saw for a larger
          // packet number.
          RCLCPP_WARN_STREAM(logger_, "Assuming remote udp_bridge restart");
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
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if(connections_.find(connection_id) != connections_.end())
    return connections_[connection_id];
  return {};
}

std::vector<std::shared_ptr<Connection> > RemoteNode::connections()
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  std::vector<std::shared_ptr<Connection> > ret;
  for(auto connection: connections_)
    if(connection.second)
      ret.push_back(connection.second);
  return ret;
}

std::shared_ptr<Connection> RemoteNode::newConnection(std::string connection_id, std::string host, uint16_t port)
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  connections_[connection_id] = std::make_shared<Connection>(connection_id, host, port);
  return connections_[connection_id];
}

void RemoteNode::adoptConnection(std::shared_ptr<Connection> connection)
{
  if(!connection)
    return;
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto& slot = connections_[connection->id()];
  if(!slot)
    slot = connection;
}


std::vector<uint8_t> RemoteNode::unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  if(message.size() < sizeof(SequencedPacketHeader) || reinterpret_cast<const SequencedPacketHeader*>(message.data())->packet_size != message.size())
  {
    RCLCPP_ERROR_STREAM(logger_, "Can't unwrap packet of size " << message.size());
    if(message.size() >= sizeof(SequencedPacketHeader))
      RCLCPP_ERROR_STREAM(logger_, "Packet reports size: " << reinterpret_cast<const SequencedPacketHeader*>(message.data())->packet_size);
    return {};
  }

  auto packet = reinterpret_cast<const SequencedPacket*>(message.data());
  std::shared_ptr<Connection> c;
  bool duplicate = false;
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    duplicate = received_packet_times_.find(packet->packet_number) != received_packet_times_.end();
    c = connection(packet->connection_id);  // re-enters mutex (recursive)
    if(!c)
      c = newConnection(packet->connection_id, source_info.host, source_info.port);
    auto now = clock_->now();
    if(!duplicate)
      received_packet_times_[packet->packet_number] = now;
  }

  // update_last_receive_time uses Connection's own mutex; safe to call
  // outside our state_mutex_.
  c->update_last_receive_time(clock_->now().seconds(), message.size(), duplicate);

  if(!duplicate)
  {
    try
    {
      return std::vector<uint8_t>(message.begin()+sizeof(SequencedPacketHeader), message.end());
    }
    catch(const std::exception& e)
    {
      RCLCPP_ERROR_STREAM(logger_, "problem decoding packet: " << e.what());
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
  topic_statistics_publisher_->publish(statistics);
}

void RemoteNode::clearReceivedPacketTimesBefore(rclcpp::Time time)
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
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

uint32_t RemoteNode::resendGiveupCount() const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return resend_giveup_count_;
}

ResendRequest RemoteNode::getMissingPackets()
{
  auto now = clock_->now();
  if(now.nanoseconds() == 0)
    return {};

  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto too_old = now - rclcpp::Duration::from_seconds(seconds(kReceiveHistoryWindow));
  clearReceivedPacketTimesBefore(too_old);  // re-enters mutex (recursive)
  auto can_resend_time = now - rclcpp::Duration::from_seconds(0.2);

  std::vector<uint64_t> missing;
  if(!received_packet_times_.empty())
    for(auto i = received_packet_times_.begin()->first+1; i < received_packet_times_.rbegin()->first; i++)
      if(received_packet_times_.find(i) == received_packet_times_.end())
        missing.push_back(i);
  ResendRequest rr;
  for(auto m: missing)
  {
    if(resend_request_times_[m].nanoseconds() == 0 || resend_request_times_[m] < can_resend_time)
    {
      rr.missing_packets.push_back(m);
      resend_request_times_[m] = now;
    }
  }
  return rr;
}

} // namespace udp_bridge

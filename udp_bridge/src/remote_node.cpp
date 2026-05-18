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
          resend_state_.clear();
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
  for(auto rs: resend_state_)
    if(rs.second.first_request_time < time)
      expired.push_back(rs.first);
  for(auto e: expired)
    resend_state_.erase(e);
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
  auto debounce_cutoff = now - rclcpp::Duration::from_seconds(seconds(kResendDebounceHold));
  auto giveup_cutoff = now - rclcpp::Duration::from_seconds(seconds(kSentPacketTTL));

  // Walk packet numbers in [first+1, last-1] of the known-received range
  // and flag gaps as missing. Each gap is debounced against the receive
  // time of its previous known neighbor: a reordered late-arriving
  // packet within kResendDebounceHold of its neighbor is given time to
  // arrive before we request a resend. prev_received_time tracks the
  // most recent receive time as we walk forward; for runs of consecutive
  // gaps it stays pinned to the packet just before the run, which is
  // the right anchor since those gaps were sent at roughly the same
  // wall-clock time.
  std::vector<uint64_t> missing;
  if(!received_packet_times_.empty())
  {
    rclcpp::Time prev_received_time = received_packet_times_.begin()->second;
    for(auto i = received_packet_times_.begin()->first+1; i < received_packet_times_.rbegin()->first; i++)
    {
      auto it = received_packet_times_.find(i);
      if(it == received_packet_times_.end())
      {
        if(prev_received_time < debounce_cutoff)
          missing.push_back(i);
      }
      else
      {
        prev_received_time = it->second;
      }
    }
  }

  // Backoff + TTL-bounded give-up. For each missing packet:
  //   - If first observed missing, request immediately; record first +
  //     last request time and attempts=1.
  //   - On repeat: cooldown = min(base * 2^(attempts-1), cap). Only
  //     request again if (now - last_request_time) > cooldown.
  //   - Give-up: if first_request_time is older than kSentPacketTTL,
  //     the sender has by then evicted the packet from sent_packets_;
  //     further requests are pointless. Bump resend_giveup_count_,
  //     remove from tracking, WARN log.
  //
  // Invariant: when multiple missing packets cross their backoff
  // boundary in the same tick, they bundle into one ResendRequest. By
  // design — don't split into per-packet requests in a future
  // refactor.
  ResendRequest rr;
  for(auto m: missing)
  {
    auto& state = resend_state_[m];
    if(state.attempts == 0)
    {
      rr.missing_packets.push_back(m);
      state.first_request_time = now;
      state.last_request_time = now;
      state.attempts = 1;
      continue;
    }
    if(state.first_request_time < giveup_cutoff)
    {
      RCLCPP_WARN_STREAM(logger_,
        "Giving up on resend of packet " << m << " from remote '"
        << name_ << "' after " << state.attempts
        << " attempts (first requested "
        << (now - state.first_request_time).seconds()
        << " s ago, sender TTL is " << seconds(kSentPacketTTL) << " s)");
      resend_state_.erase(m);
      ++resend_giveup_count_;
      continue;
    }
    double cooldown = seconds(kResendBackoffBase) * (1u << (state.attempts - 1));
    if(cooldown > seconds(kResendBackoffCap))
      cooldown = seconds(kResendBackoffCap);
    if((now - state.last_request_time).seconds() >= cooldown)
    {
      rr.missing_packets.push_back(m);
      state.last_request_time = now;
      ++state.attempts;
    }
  }
  return rr;
}

} // namespace udp_bridge

// Locking convention (see udp_bridge.h members):
//
//   - publishers_mutex_, subscribers_mutex_, remote_nodes_mutex_,
//     pending_connections_mutex_ guard the four maps that cross
//     callback-group boundaries under MultiThreadedExecutor.
//   - When more than one of these is needed in a single function, use
//     std::scoped_lock to acquire them deadlock-free in one shot. No
//     manual lock-ordering required.
//   - Pattern: lock briefly, copy out shared_ptrs / values needed, then
//     release the lock before slow operations (publish, sendto, recvfrom,
//     create_generic_publisher, get_publishers_info_by_topic,
//     create_generic_subscription). For check-then-create patterns
//     (decodeData, updateLocalSubscriptions), use a three-phase
//     lookup: phase 1 check map under lock, phase 2 do the slow rmw
//     call without lock, phase 3 re-acquire and try-emplace with
//     race-guard.
//   - Helpers in this file (sendBridgeInfo, allRemotes, sendConnectionRequests,
//     addSubscriberConnection, the send() template specialization) acquire
//     their own locks; callers do NOT pre-lock. Each entry-point caller
//     (timer, subscription, service handler, decode handler) is otherwise
//     responsible for taking locks before touching the maps directly.
//   - Connection has its own per-instance mutexes (sent_packets_mutex_,
//     sent_packet_statistics_mutex_, receive_history_mutex_) for state
//     owned by the Connection.
//   - RemoteNode has its own state_mutex_ (recursive_mutex) guarding
//     connections_, received_packet_times_, resend_request_times_,
//     next_packet_number_, last_packet_time_. Its public methods acquire
//     the lock; some call other public methods, hence recursive_mutex.
//     defragmenter_ is intentionally NOT guarded — by contract it's only
//     touched from socket_drain_group_ (UDPBridge::decode for Fragment
//     packets and the spin_once tail cleanup loop).
//   - Protocol counters next_packet_number_, next_fragmented_packet_id_,
//     next_connection_internal_message_sequence_number_, and
//     last_packet_number_assign_time_ns_ are std::atomic. Each is read
//     and written from multiple callback groups via send<>(); fetch_add(1)
//     gives a unique value per call. Duplicate packet numbers would
//     corrupt the resend protocol, so atomicity is load-bearing here.
//   - name_, port_, max_packet_size_ are set once in on_configure and
//     read-only afterward — no synchronization needed.

#include "udp_bridge/udp_bridge.h"
#include "udp_bridge/qos_resolution.h"

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/serialization.hpp"

#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <unordered_set>

#include "udp_bridge_interfaces/msg/remote_subscribe_internal.hpp"
#include "udp_bridge_interfaces/msg/message_internal.hpp"
#include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge_interfaces/msg/resend_request.hpp"
#include "udp_bridge/remote_node.h"
#include "udp_bridge/types.h"
#include "udp_bridge/utilities.h"
#include "lifecycle_msgs/msg/state.hpp"

namespace udp_bridge
{

using namespace udp_bridge_interfaces::msg;
using namespace udp_bridge_interfaces::srv;
using namespace std::placeholders;
using namespace std::chrono_literals;

UDPBridge::UDPBridge(const std::string &node_name)
: rclcpp_lifecycle::LifecycleNode(node_name, rclcpp::NodeOptions().enable_logger_service(true))
{
}

UDPBridge::CallbackReturn UDPBridge::on_configure(const rclcpp_lifecycle::State & state)
{
  // start with the ROS2 node name
  std::string name = get_name();
  auto last_slash = name.rfind('/');
  if(last_slash != std::string::npos)
    name = name.substr(last_slash+1);
  declare_parameter( "name", name);
  // This is the name of the UDPBridge node, not the ROS2 node name
  setName(get_parameter("name").as_string());


  RCLCPP_INFO_STREAM(get_logger(), "name: " << name_);

  declare_parameter("port", port_);
  port_ = get_parameter("port").as_int();
  RCLCPP_INFO_STREAM(get_logger(), "port: " << port_); 

  declare_parameter("maximum_packet_size", max_packet_size_);
  max_packet_size_ = get_parameter("maximum_packet_size").as_int();
  RCLCPP_INFO_STREAM(get_logger(), "maximum_packet_size: " << max_packet_size_);

  //maximum_packet_size_subscriber_ = ros::NodeHandle("~").subscribe("maximum_packet_size", 1, &UDPBridge::maximumPacketSizeCallback, this);

  socket_ = socket(AF_INET, SOCK_DGRAM, 0);
  if(socket_ < 0)
  {
    RCLCPP_ERROR(get_logger(),"Failed creating socket");
    exit(1);
  }
    
  sockaddr_in bind_address; // address to listen on
  memset((char *)&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = htons(port_);
  
  if(bind(socket_, (sockaddr*)&bind_address, sizeof(bind_address)) < 0)
  {
    RCLCPP_ERROR(get_logger(), "Error binding socket");
    exit(1);
  }
  
  timeval socket_timeout;
  socket_timeout.tv_sec = 0;
  socket_timeout.tv_usec = 1000;
  if(setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) < 0)
  {
    RCLCPP_ERROR(get_logger(), "Error setting socket timeout");
    exit(1);
  }

  int buffer_size;
  unsigned int s = sizeof(buffer_size);
  getsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "recv buffer size:" << buffer_size);
  buffer_size = 500000;
  setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "recv buffer size set to:" << buffer_size);
  buffer_size = 500000;
  setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(socket_, SOL_SOCKET, SO_SNDBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "send buffer size set to:" << buffer_size);

  // Callback groups — invariants documented in udp_bridge_node.cpp.
  socket_drain_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  republish_group_    = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  periodic_group_     = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  std::string node_name = get_name();
  // Service handlers are admin-style work; assign to the periodic group so they
  // serialize against other admin work but never starve the socket-drain group.
  const rclcpp::QoS service_qos = rclcpp::ServicesQoS();
  subscribe_service_ = create_service<Subscribe>(
    node_name+"/remote_subscribe",
    std::bind(&UDPBridge::remoteSubscribe, this, _1, _2),
    service_qos, periodic_group_);
  advertise_service_ = create_service<Subscribe>(
    node_name+"/remote_advertise",
    std::bind(&UDPBridge::remoteAdvertise, this, _1, _2),
    service_qos, periodic_group_);

  add_remote_service_ = create_service<AddRemote>(
    node_name+"/add_remote",
    std::bind(&UDPBridge::addRemote, this, _1, _2),
    service_qos, periodic_group_);
  list_remotes_service_ = create_service<ListRemotes>(
    node_name+"/list_remotes",
    std::bind(&UDPBridge::listRemotes, this, _1, _2),
    service_qos, periodic_group_);
  
  topic_statistics_publisher_ = create_publisher<TopicStatisticsArray>(node_name+"/topic_statistics",10);

  rclcpp::QoS latching_qos(1);
  latching_qos.transient_local();
  latching_qos.keep_last(1);

  bridge_info_publisher_ = create_publisher<BridgeInfo>(node_name+"/bridge_info", latching_qos);

  declare_parameter("remotes_list", std::vector<std::string>());
  auto remotes_list = get_parameter("remotes_list").as_string_array();
  for(auto remote_name: remotes_list)
  {
    Remote remote_info;
    remote_info.name = remote_name;
    remote_nodes_[remote_info.name] = std::make_shared<RemoteNode>(remote_info.name, name_, *this);
    remote_nodes_[remote_info.name]->update(remote_info);


    std::string connections_list_param = "remotes."+remote_name+".connections_list";
    declare_parameter(connections_list_param, std::vector<std::string>());
    auto connections_list = get_parameter(connections_list_param).as_string_array();
    for(auto connection_name: connections_list)
    {
      RemoteConnection connection;
      connection.connection_id = connection_name;
      
      std::string host_param = "remotes." + remote_name + ".connections." + connection_name + ".host";
      declare_parameter(host_param, "");
      connection.host = get_parameter(host_param).as_string();

      std::string port_param = "remotes." + remote_name + ".connections." + connection_name + ".port";
      declare_parameter(port_param, 0);
      connection.port = get_parameter(port_param).as_int();
      
      std::string return_host_param = "remotes." + remote_name + ".connections." + connection_name + ".return_host";
      declare_parameter(return_host_param, "");
      connection.return_host = get_parameter(return_host_param).as_string();

      std::string return_port_param = "remotes." + remote_name + ".connections." + connection_name + ".return_port";
      declare_parameter(return_port_param, 0);
      connection.return_port = get_parameter(return_port_param).as_int();

      std::string maximum_bytes_per_second_param = "remotes." + remote_name + ".connections." + connection_name + ".maximum_bytes_per_second";
      declare_parameter(maximum_bytes_per_second_param, 0);
      int mbps = get_parameter(maximum_bytes_per_second_param).as_int();
      if(mbps > 0)
        connection.maximum_bytes_per_second = mbps;

      remote_info.connections.push_back(connection);
      remote_nodes_[remote_info.name]->update(remote_info);

      std::string topics_list_param = "remotes." + remote_name + ".connections." + connection_name + ".topics_list";
      declare_parameter(topics_list_param, std::vector<std::string>());
      auto topics_list = get_parameter(topics_list_param).as_string_array();
      for(auto topic: topics_list)
      {
        std::string queue_size_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".queue_size";
        declare_parameter(queue_size_param, 10);
        int queue_size = get_parameter(queue_size_param).as_int();

        std::string period_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".period";
        declare_parameter(period_param, 0.0);
        double period = get_parameter(period_param).as_double();

        std::string source_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".source";
        declare_parameter(source_param, topic);
        std::string source = get_parameter(source_param).as_string();
        source = get_node_base_interface()->resolve_topic_or_service_name(source, false, false);

        std::string destination_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".destination";
        declare_parameter(destination_param, source);
        auto destination = get_parameter(destination_param).as_string();

        std::string reliability_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".reliability";
        declare_parameter(reliability_param, std::string());
        std::string reliability = get_parameter(reliability_param).as_string();

        std::string durability_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".durability";
        declare_parameter(durability_param, std::string());
        std::string durability = get_parameter(durability_param).as_string();

        std::string history_depth_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".history_depth";
        declare_parameter(history_depth_param, 0);
        uint32_t history_depth = static_cast<uint32_t>(get_parameter(history_depth_param).as_int());

        addSubscriberConnection(source, destination, queue_size, period,
                                remote_info.name, connection.connection_id,
                                reliability, durability, history_depth);
      }
    }
  }
  
  
  stats_report_timer_ = create_wall_timer(
    1s, std::bind(&UDPBridge::statsReportCallback, this), periodic_group_);
  bridge_info_timer_ = create_wall_timer(
    2s, std::bind(&UDPBridge::bridgeInfoCallback, this), periodic_group_);
  // Hot path: socket drainer must never be starved. Owns its own group.
  spin_timer_ = create_wall_timer(
    10ms, std::bind(&UDPBridge::spin_once, this), socket_drain_group_);
  subscription_update_timer_ = create_wall_timer(
    1s, std::bind(&UDPBridge::updateLocalSubscriptions, this), periodic_group_);

  diagnostic_updater_ = std::make_unique<diagnostic_updater::Updater>(this);
  diagnostic_updater_->setHardwareID(name_);
  syncDiagnosticTasks();
  diagnostic_timer_ = create_wall_timer(
    1s, std::bind(&UDPBridge::diagnosticTick, this), periodic_group_);

  return LifecycleNode::on_configure(state);
}

UDPBridge::CallbackReturn UDPBridge::on_activate(const rclcpp_lifecycle::State & state)
{
  return LifecycleNode::on_activate(state);
}

UDPBridge::CallbackReturn UDPBridge::on_deactivate(const rclcpp_lifecycle::State & state)
{
  return LifecycleNode::on_deactivate(state);
}

UDPBridge::CallbackReturn UDPBridge::on_cleanup(const rclcpp_lifecycle::State & state)
{
  diagnostic_timer_.reset();
  diagnostic_updater_.reset();
  diagnostic_task_names_.clear();
  return LifecycleNode::on_cleanup(state);
}

UDPBridge::CallbackReturn UDPBridge::on_shutdown(const rclcpp_lifecycle::State & state)
{
  return LifecycleNode::on_shutdown(state);
}


void UDPBridge::setName(const std::string &name)
{
  name_ = name;
  if(name_.size() >= maximum_node_name_size-1)
  {
    name_ = name_.substr(0, maximum_node_name_size-1);
    RCLCPP_WARN_STREAM(get_logger(), "udp_bridge name truncated to " << name_);
  }
}

void UDPBridge::spin_once()
{
  if (get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    // If the node is not active, we don't need to process incoming packets.
    // This is important for the lifecycle node to avoid processing packets
    // when the node is not in the active state.
    return;
  }

  sockaddr_in remote_address;
  socklen_t remote_address_length = sizeof(remote_address);
  while(true)
  {
    pollfd p;
    p.fd = socket_;
    p.events = POLLIN;
    int ret = poll(&p, 1, 10);
    if(ret < 0)
      RCLCPP_WARN_STREAM(get_logger(), "poll error: " << int(errno) << " " << strerror(errno));
    if(ret > 0 && p.revents & POLLIN)
    {
      int receive_length = 0;
      std::vector<uint8_t> buffer;
      int buffer_size;
      unsigned int buffer_size_size = sizeof(buffer_size);
      getsockopt(socket_,SOL_SOCKET,SO_RCVBUF,&buffer_size,&buffer_size_size);
      buffer.resize(buffer_size);
      receive_length = recvfrom(socket_, &buffer.front(), buffer_size, 0, (sockaddr*)&remote_address, &remote_address_length);
      RCLCPP_DEBUG_STREAM(get_logger(), "received " << receive_length << " bytes");
      if(receive_length > 0)
      {
        SourceInfo source_info;
        source_info.host = addressToDotted(remote_address);
        source_info.port = ntohs(remote_address.sin_port);
        buffer.resize(receive_length);
        decode(buffer, source_info);
      }
    }
    else
      break;
  }
  // Snapshot remotes for the per-remote defragmenter cleanup.
  std::vector<std::pair<std::string, std::shared_ptr<RemoteNode>>> remotes;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    remotes.assign(remote_nodes_.begin(), remote_nodes_.end());
  }
  auto cleanup_cutoff = get_clock()->now() - rclcpp::Duration::from_seconds(5);
  for(auto& remote: remotes)
  {
    if(remote.second)
    {
      int discard_count = remote.second->defragmenter().cleanup(cleanup_cutoff);
      if(discard_count)
        RCLCPP_INFO_STREAM(get_logger(), "Discarded " << discard_count << " incomplete packets from " << remote.first);
    }
  }
  cleanupSentPackets();
  resendMissingPackets();
}

void UDPBridge::callback(std::string topic_name, std::string topic_type, std::shared_ptr<rclcpp::SerializedMessage> message)
{
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    return;
  }

  rclcpp::Time now = get_clock()->now();

  // Read remote_details / connection_rates and update last_sent_time + initial
  // statistics under subscribers_mutex_. We also capture per-destination
  // destination_topic and QoS config so the send loop below doesn't need to
  // re-lock.
  struct DestinationConfig
  {
    std::string destination_topic;
    std::string reliability;
    std::string durability;
    uint32_t history_depth = 0;
  };
  RemoteConnectionsList destinations;
  std::map<std::string, DestinationConfig> destination_config_by_remote;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    auto& sub = subscribers_[topic_name];
    for(auto& remote_details: sub.remote_details)
    {
      std::unordered_set<float> periods; // group the sending to connections with same period
      for(auto& connection_rate: remote_details.second.connection_rates)
        if(connection_rate.second.period >= 0)
          if(connection_rate.second.period == 0 || connection_rate.second.last_sent_time.nanoseconds() == 0 || now-connection_rate.second.last_sent_time > rclcpp::Duration::from_seconds(connection_rate.second.period) || periods.count(connection_rate.second.period) > 0)
          {
            destinations[remote_details.first].push_back(connection_rate.first);
            connection_rate.second.last_sent_time = now;
            if(periods.count(connection_rate.second.period) == 0)
              periods.insert(connection_rate.second.period);
          }
      auto& dc = destination_config_by_remote[remote_details.first];
      dc.destination_topic = remote_details.second.destination_topic;
      dc.reliability       = remote_details.second.reliability;
      dc.durability        = remote_details.second.durability;
      dc.history_depth     = remote_details.second.history_depth;
    }

    MessageSizeData size_data;
    size_data.message_size = message->size();
    size_data.timestamp = now;
    size_data.send_results[""][""];
    sub.statistics.add(size_data);
  }

  if (destinations.empty())
  {
    RCLCPP_DEBUG_STREAM(get_logger(), "No ready destination to send message");
    return;
  }

  // First, serialize message in a MessageInternal message
  MessageInternal message_internal;

  message_internal.source_topic = topic_name;
  message_internal.datatype = topic_type;
  //message.md5sum = msg->getMD5Sum();
  //message.message_definition = msg->getMessageDefinition();


  message_internal.data.resize(message->size());
  memcpy(message_internal.data.data(), message->get_rcl_serialized_message().buffer, message->size());

  // Send (slow — acquires remote_nodes_mutex_ / pending_connections_mutex_
  // internally). Updating per-destination statistics afterward locks
  // subscribers_mutex_ briefly per destination.
  for(auto destination: destinations)
  {
    auto config_it = destination_config_by_remote.find(destination.first);
    if(config_it != destination_config_by_remote.end())
    {
      message_internal.destination_topic = config_it->second.destination_topic;
      message_internal.reliability       = config_it->second.reliability;
      message_internal.durability        = config_it->second.durability;
      message_internal.history_depth     = config_it->second.history_depth;
    }
    else
    {
      message_internal.destination_topic.clear();
      message_internal.reliability.clear();
      message_internal.durability.clear();
      message_internal.history_depth = 0;
    }
    RemoteConnectionsList connections;
    connections[destination.first] = destination.second;
    auto size_data = send(message_internal, connections, false);
    size_data.message_size = message->size();
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      subscribers_[topic_name].statistics.add(size_data);
    }
  }
}

void UDPBridge::decode(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  if(message.empty())
  {
    RCLCPP_DEBUG_STREAM(get_logger(), "empty message from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
    return;
  }

  const Packet *packet = reinterpret_cast<const Packet*>(message.data());
  RCLCPP_DEBUG_STREAM(get_logger(), "Received packet of type " << int(packet->type) << " and size " << message.size() << " from '" << source_info.node_name << "' (" << source_info.host << ":" << source_info.port << ")");
  std::shared_ptr<RemoteNode> remote_node;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote_node_iterator = remote_nodes_.find(source_info.node_name);
    if(remote_node_iterator != remote_nodes_.end())
      remote_node = remote_node_iterator->second;
  }
  switch(packet->type)
  {
    case PacketType::Data:
      decodeData(message, source_info);
      break;
    case PacketType::Compressed:
      decode(uncompress(message), source_info);
      break;
    case PacketType::SubscribeRequest:
      decodeSubscribeRequest(message, source_info);
      break;
    case PacketType::Fragment:
      if(remote_node)
        if(remote_node->defragmenter().addFragment(message, get_clock()->now()))
          for(auto p: remote_node->defragmenter().getPackets())
            decode(p, source_info);
      break;
    case PacketType::BridgeInfo:
        decodeBridgeInfo(message, source_info);
        break;
    case PacketType::TopicStatistics:
        decodeTopicStatistics(message, source_info);
        break;
    case PacketType::WrappedPacket:
      unwrap(message, source_info);
      break;
    case PacketType::ResendRequest:
      decodeResendRequest(message, source_info);
      break;
    case PacketType::Connection:
      decodeConnection(message, source_info);
      break;
    default:
        RCLCPP_WARN_STREAM(get_logger(), "Unknown packet type: " << int(packet->type) << " from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
  }
}

void UDPBridge::decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  (void)source_info;
  auto outer_message = deserialize<MessageInternal>(message);

  rclcpp::SerializedMessage serialized_message;
  serialized_message.reserve(outer_message.data.size());
  memcpy(serialized_message.get_rcl_serialized_message().buffer, outer_message.data.data(), outer_message.data.size());
  serialized_message.get_rcl_serialized_message().buffer_length = outer_message.data.size();

  // publish, but first advertise if publisher not present
  auto topic = outer_message.destination_topic;
  if(topic.empty())
    topic = outer_message.source_topic;

  // Three-phase lookup so create_generic_publisher() runs WITHOUT
  // publishers_mutex_ held. Holding the lock across rmw discovery /
  // type-support resolution would block every other receive-decode
  // path on first-arrival creation — exactly the wedge mechanism this
  // commit is hardening against.
  rclcpp::GenericPublisher::SharedPtr publisher;
  bool first_arrival = false;

  // Phase 1: check map under lock.
  {
    std::lock_guard<std::mutex> lock(publishers_mutex_);
    auto it = publishers_.find(topic);
    if(it != publishers_.end())
      publisher = it->second;
  }

  // Phase 2: if missing, do the slow rmw call without holding the lock.
  if(!publisher)
  {
    // Resolve per-topic QoS from MessageInternal (sender-advertised),
    // falling back to package defaults when fields are empty/zero —
    // see udp_bridge/doc/qos_design.md for the contract.
    auto qos = resolveDestinationPublisherQos(
      outer_message.reliability,
      outer_message.durability,
      outer_message.history_depth);
    auto new_publisher = create_generic_publisher(topic, outer_message.datatype, qos);

    // Phase 3: re-acquire and try-emplace. If another thread beat us
    // (raced and inserted first), we use theirs and discard ours.
    {
      std::lock_guard<std::mutex> lock(publishers_mutex_);
      auto [it, inserted] = publishers_.try_emplace(topic, new_publisher);
      publisher = it->second;
      first_arrival = inserted;
    }
  }

  // sendBridgeInfo and publish run without publishers_mutex_ held —
  // both are slow operations that should not block the publishers_ map.
  if(first_arrival)
    sendBridgeInfo();

  publisher->publish(serialized_message);
}

void UDPBridge::decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto bridge_info = deserialize<BridgeInfo>(message);

  std::shared_ptr<RemoteNode> remote_node;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    remote_node = remote_nodes_[source_info.node_name];
    if(!remote_node)
    {
      remote_node = std::make_shared<RemoteNode>(source_info.node_name, name_, *this);
      remote_nodes_[source_info.node_name] = remote_node;
    }
  }
  remote_node->update(bridge_info, source_info);
}

void UDPBridge::decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto rr = deserialize<ResendRequest>(message);
  auto now = get_clock()->now();

  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote_node = remote_nodes_.find(source_info.node_name);
    if(remote_node != remote_nodes_.end() && remote_node->second)
      connections = remote_node->second->connections();
  }
  // resend_packets does network I/O — call without remote_nodes_mutex_ held.
  for(auto& connection: connections)
    if(connection)
      connection->resend_packets(rr.missing_packets, socket_, now);
}

void UDPBridge::decodeTopicStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto topic_statistics = deserialize<TopicStatisticsArray>(message);

  std::shared_ptr<RemoteNode> remote_node;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto it = remote_nodes_.find(source_info.node_name);
    if(it != remote_nodes_.end())
      remote_node = it->second;
  }
  if(remote_node)
    remote_node->publishTopicStatistics(topic_statistics);
}


void UDPBridge::addSubscriberConnection(std::string const &source_topic,
                                        std::string const &destination_topic,
                                        uint32_t queue_size, float period,
                                        std::string remote_node,
                                        std::string connection_id,
                                        std::string reliability,
                                        std::string durability,
                                        uint32_t history_depth)
{
  if(!remote_node.empty())
  {
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      auto& sub = subscribers_[source_topic];
      sub.queue_size = std::max(queue_size, uint32_t(1));
      auto& rd = sub.remote_details[remote_node];
      rd.destination_topic = destination_topic;
      rd.connection_rates[connection_id].period = period;
      // QoS overrides — empty/zero retains existing or default behavior so
      // that re-adding a subscription doesn't accidentally clear earlier
      // per-topic QoS that was set elsewhere.
      if(!reliability.empty())
        rd.reliability = reliability;
      if(!durability.empty())
      {
        rd.durability = durability;
        // Source-side subscription durability follows the strongest
        // per-topic setting across remotes for this source topic.
        // transient_local overrides volatile so a latched source is
        // received even if only one consumer needs it.
        if(durability == "transient_local")
          sub.durability = "transient_local";
      }
      if(history_depth != 0)
        rd.history_depth = history_depth;
    }
    // sendBridgeInfo runs without subscribers_mutex_ held (it acquires
    // its own locks). Calling it inside the locked scope above would
    // deadlock since it re-acquires subscribers_mutex_.
    sendBridgeInfo();
  }
}

void UDPBridge::updateLocalSubscriptions()
{
  // Three-phase lookup so get_publishers_info_by_topic() and
  // create_generic_subscription() (both slow rmw calls) run WITHOUT
  // subscribers_mutex_ held. Holding the lock across them would block
  // callback() (republish-group reader) and statsReportCallback /
  // bridgeInfoCallback (periodic-group readers).
  struct PendingCreate
  {
    std::string source_topic;
    std::string durability;
    uint32_t queue_size;
  };

  // Phase 1: snapshot which topics need a new subscription, under lock.
  std::vector<PendingCreate> pending;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for(auto& subscriber: subscribers_)
      if(!subscriber.second.subscription)
        pending.push_back({subscriber.first,
                           subscriber.second.durability,
                           subscriber.second.queue_size});
  }

  // Phase 2: do the slow rmw work for each, without the lock.
  for(const auto& p: pending)
  {
    auto info = get_publishers_info_by_topic(p.source_topic);
    if(info.empty())
      continue;
    std::string topic_type = info.front().topic_type();
    auto source_topic = p.source_topic;
    auto cb = [this, source_topic, topic_type](std::shared_ptr<rclcpp::SerializedMessage> message)
    {
      this->callback(source_topic, topic_type, message);
    };

    // Source-side QoS: best_effort (accepts any publisher) plus
    // per-topic durability (transient_local when configured, so latched
    // source values reach the bridge). See qos_design.md.
    auto qos = resolveSourceSubscriptionQos(p.durability, p.queue_size);

    rclcpp::SubscriptionOptions options;
    options.ignore_local_publications = true;
    // Forwarding subscriptions run in republish_group_ so a stalled
    // remote publisher cannot starve the socket-drain group.
    options.callback_group = republish_group_;
    auto subscription = create_generic_subscription(source_topic, topic_type, qos, cb, options);

    // Phase 3: re-acquire and assign — but only if no one beat us. Race
    // guard: a concurrent updateLocalSubscriptions tick or
    // addSubscriberConnection could have already created the
    // subscription; in that case we drop ours.
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      auto it = subscribers_.find(p.source_topic);
      if(it != subscribers_.end() && !it->second.subscription)
        it->second.subscription = subscription;
    }
  }
}

void UDPBridge::decodeSubscribeRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto remote_request = deserialize<RemoteSubscribeInternal>(message);

  addSubscriberConnection(remote_request.source_topic, remote_request.destination_topic, remote_request.queue_size, remote_request.period, source_info.node_name, remote_request.connection_id);
}

void UDPBridge::unwrap(const std::vector<uint8_t>& message, const SourceInfo& source_info)
{
  if(message.size() > sizeof(SequencedPacketHeader))
  {
    const SequencedPacket* wrapped_packet = reinterpret_cast<const SequencedPacket*>(message.data());

    auto updated_source_info = source_info;
    updated_source_info.node_name = wrapped_packet->source_node;

    std::shared_ptr<RemoteNode> remote;
    {
      std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
      auto remote_iterator = remote_nodes_.find(wrapped_packet->source_node);
      if(remote_iterator != remote_nodes_.end())
        remote = remote_iterator->second;

      if(!remote)
      {
        if(wrapped_packet->source_node == name_)
        {
          RCLCPP_ERROR_STREAM(get_logger(), "Received a packet from a node with our name: " << name_ << " connection: " << wrapped_packet->connection_id << " host: " << source_info.host << " port: " << source_info.port);
          return;
        }
        remote = std::make_shared<RemoteNode>(wrapped_packet->source_node, name_, *this);
        remote_nodes_[wrapped_packet->source_node] = remote;
      }
    }
    // remote->unwrap and the recursive decode() run without remote_nodes_mutex_
    // held; decode() will reacquire it for any RemoteNode lookups it needs.
    try
    {
      decode(remote->unwrap(message, updated_source_info), updated_source_info);
    }
    catch(const std::exception& e)
    {
      RCLCPP_ERROR_STREAM(get_logger(), "problem decoding packet: " << e.what());
    }
  }
  else
    RCLCPP_ERROR_STREAM(get_logger(), "Incomplete wrapped packet of size: " << message.size());
}

void UDPBridge::decodeConnection(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto connection_internal = deserialize<ConnectionInternal>(message);

  switch(connection_internal.operation)
  {
    case ConnectionInternal::OPERATION_CONNECT:
      {
        {
          std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
          auto remote = remote_nodes_[source_info.node_name];
          if(!remote)
          {
            remote = std::make_shared<RemoteNode>(source_info.node_name, name_, *this);
            remote_nodes_[source_info.node_name] = remote;
          }
          uint16_t port = source_info.port;
          if(connection_internal.return_port != 0)
            port = connection_internal.return_port;
          auto host = source_info.host;
          if(!connection_internal.return_host.empty())
            host = connection_internal.return_host;
          auto connection = remote->connection(connection_internal.connection_id);
          if(!connection)
          {
            connection = remote->newConnection(connection_internal.connection_id, host, port);
            remote->connections().push_back(connection);
          }
          else
            connection->setHostAndPort(host, port);
          if(connection_internal.return_maximum_bytes_per_second > 0)
            connection->setRateLimit(connection_internal.return_maximum_bytes_per_second);
          connection_internal.operation = ConnectionInternal::OPERATION_CONNECT_ACKNOWLEDGE;
          connection_internal.return_host = connection->returnHost();
          connection_internal.return_port = connection->returnPort();
        }
        // send() acquires its own locks; call without remote_nodes_mutex_.
        send(connection_internal, source_info.node_name, true);
      }
      break;
    case ConnectionInternal::OPERATION_CONNECT_ACKNOWLEDGE:
      {
        std::scoped_lock lock(remote_nodes_mutex_, pending_connections_mutex_);
        if(pending_connections_.find(connection_internal.sequence_number) != pending_connections_.end())
        {
          auto remote = remote_nodes_[source_info.node_name];
          if(!remote)
          {
            remote = std::make_shared<RemoteNode>(source_info.node_name, name_, *this);
            remote_nodes_[source_info.node_name] = remote;
          }
          auto connection = remote->connection(connection_internal.connection_id);
          auto pending_connection = pending_connections_[connection_internal.sequence_number].connection;
          if(pending_connection)
          {
            if(!connection)
            {
              connection = pending_connection;
              remote->connections().push_back(connection);
            }
          }
          pending_connections_.erase(connection_internal.sequence_number);
        }
      }
      break;
    default:
      RCLCPP_WARN_STREAM(get_logger(), "Unhandled ConnectionInternal operation type: " << connection_internal.operation);
  }
}


void UDPBridge::resendMissingPackets()
{
  // Snapshot remotes under the lock; getMissingPackets and send() run unlocked.
  std::vector<std::pair<std::string, std::shared_ptr<RemoteNode>>> remotes;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    remotes.assign(remote_nodes_.begin(), remote_nodes_.end());
  }
  for(auto remote: remotes)
    if(remote.second)
    {
      auto rr = remote.second->getMissingPackets();
      if(!rr.missing_packets.empty())
      {
        RCLCPP_DEBUG_STREAM(get_logger(), "Sending a request to resend " << rr.missing_packets.size() << " packets");
        send(rr, remote.first, true);
      }
    }
}


template <typename MessageType> MessageSizeData UDPBridge::send(MessageType const &message, const RemoteConnectionsList& remotes, bool is_overhead)
{
  rclcpp::Serialization<MessageType> serializer;
  rclcpp::SerializedMessage serialized_message;
  serializer.serialize_message(&message, &serialized_message);

  auto serial_size = serialized_message.size();
    
  std::vector<uint8_t> packet_data(sizeof(PacketHeader)+serial_size);
  Packet * packet = reinterpret_cast<Packet *>(packet_data.data());
  memcpy(&packet->data, serialized_message.get_rcl_serialized_message().buffer, serial_size);
  packet->type = packetTypeOf(message);

  packet_data = compress(packet_data);

  auto fragments = fragment(packet_data);

  auto size_data = send(fragments, remotes, is_overhead);

  size_data.message_size = serial_size;
  size_data.fragment_count = fragments.size();
  return size_data;
}

template <typename MessageType>
MessageSizeData UDPBridge::send(MessageType const &message, const std::string& remote, bool is_overhead)
{
  RemoteConnectionsList rcl;
  rcl[remote];
  return send(message, rcl, is_overhead);
}


// wrap and send a series of packets that should be sent as a group, such as fragments.
template<> MessageSizeData UDPBridge::send(const std::vector<std::vector<uint8_t> >& data, const RemoteConnectionsList& remotes, bool is_overhead)
{
  MessageSizeData size_data;
  std::vector<WrappedPacket> wrapped_packets;

  auto now = get_clock()->now();

  for(auto packet: data)
  {
    // fetch_add(1) gives a unique packet number under concurrent calls
    // from multiple callback groups (republish/socket-drain/periodic).
    uint64_t packet_number = next_packet_number_.fetch_add(1);
    last_packet_number_assign_time_ns_.store(now.nanoseconds());
    wrapped_packets.push_back(WrappedPacket(packet_number, packet, now));
    size_data.sent_size += wrapped_packets.back().packet.size();
  }

  if(wrapped_packets.empty())
    return size_data;

  size_data.timestamp = wrapped_packets.front().timestamp;

  // Resolve all (remote, connections) under the maps' locks, copy out
  // shared_ptrs, then release before doing the actual send (which performs
  // network I/O and can stall).
  std::map<std::string, std::vector<std::shared_ptr<Connection>>> connections_by_remote;
  {
    std::scoped_lock lock(remote_nodes_mutex_, pending_connections_mutex_);
    for(auto remote: remotes)
    {
      std::vector<std::shared_ptr<Connection>> connections;
      if(remote.first.empty()) // connection request?
      {
        for(auto sequence_number_str: remote.second)
          for(auto pending_connection: pending_connections_)
            if(std::to_string(pending_connection.first) == sequence_number_str)
              connections.push_back(pending_connection.second.connection);
      }
      else
      {
        auto remote_node = remote_nodes_.find(remote.first);
        if(remote_node != remote_nodes_.end())
        {
          if(remote.second.empty())
            connections = remote_node->second->connections();
          else
            for(auto connection_id: remote.second)
              connections.push_back(remote_node->second->connection(connection_id));
        }
      }
      connections_by_remote[remote.first] = std::move(connections);
    }
  }

  for(auto& entry: connections_by_remote)
  {
    for(auto connection: entry.second)
      if(connection)
      {
        auto result = connection->send(wrapped_packets, socket_, name_, is_overhead, now);
        size_data.send_results[entry.first][connection->id()] = result;
      }
  }

  return size_data;
}

void UDPBridge::cleanupSentPackets()
{
  auto now = get_clock()->now();
  if(now.nanoseconds() == 0)
    return;
  auto old_enough = now - rclcpp::Duration::from_seconds(3.0);

  // Snapshot connections under the lock; cleanup_sent_packets takes its own
  // per-Connection mutex.
  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    for(auto& remote: remote_nodes_)
      if(remote.second)
        for(auto connection: remote.second->connections())
          if(connection)
            connections.push_back(connection);
  }
  for(auto& connection: connections)
    connection->cleanup_sent_packets(old_enough);
}


void UDPBridge::remoteSubscribe(
  const std::shared_ptr<udp_bridge::Subscribe::Request> request,
  std::shared_ptr<udp_bridge::Subscribe::Response> response)
{
  RCLCPP_INFO_STREAM(get_logger(), "subscribe: remote: " << request->remote << ":" << " connection: " << request->connection_id << " source topic: " << request->source_topic << " destination topic: " << request->destination_topic);
  
  udp_bridge::RemoteSubscribeInternal remote_request;
  remote_request.source_topic = request->source_topic;
  remote_request.destination_topic = request->destination_topic;
  remote_request.queue_size = request->queue_size;
  remote_request.period = request->period;
  remote_request.connection_id = request->connection_id;

  send(remote_request, request->remote, true);
  
}

void UDPBridge::remoteAdvertise(
  const std::shared_ptr<udp_bridge::Subscribe::Request> request,
  std::shared_ptr<udp_bridge::Subscribe::Response> response)
{
  (void)response;
  std::shared_ptr<Connection> connection;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote = remote_nodes_.find(request->remote);
    if(remote == remote_nodes_.end())
      return;
    connection = remote->second->connection(request->connection_id);
  }
  if(!connection)
    return;

  // addSubscriberConnection acquires its own locks (and calls sendBridgeInfo
  // internally); calling sendBridgeInfo a second time here is redundant.
  addSubscriberConnection(request->source_topic, request->destination_topic, request->queue_size, request->period, request->remote, request->connection_id);
}

void UDPBridge::statsReportCallback()
{
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    return;
  }

  TopicStatisticsArray tsa;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for(auto& subscriber: subscribers_)
    {
      for(auto ts: subscriber.second.statistics.get())
      {
        ts.source_node = name_;
        ts.source_topic = subscriber.first;
        auto details = subscriber.second.remote_details.find(ts.destination_node);
        if(details != subscriber.second.remote_details.end())
          ts.destination_topic = details->second.destination_topic;
        tsa.topics.push_back(ts);
      }
    }
  }

  topic_statistics_publisher_->publish(tsa);

  send(tsa, allRemotes(), true);
}

std::vector<std::vector<uint8_t> > UDPBridge::fragment(const std::vector<uint8_t>& data)
{
  std::vector<std::vector<uint8_t> > ret;

  unsigned long max_fragment_payload_size = max_packet_size_-(sizeof(FragmentHeader)+sizeof(SequencedPacketHeader));
  if(data.size()+sizeof(SequencedPacketHeader) > max_packet_size_)
  {
    // Reserve a unique packet_id atomically. Under concurrent fragment()
    // calls from multiple callback groups, two messages could otherwise
    // both read N, both assign N to their fragments, and both increment —
    // breaking defragmentation on the receiver. fetch_add(1) gives each
    // call its own ID up front.
    const uint32_t this_packet_id = next_fragmented_packet_id_.fetch_add(1);
    for(unsigned long i = 0; i < data.size(); i += max_fragment_payload_size)
    {
      unsigned long fragment_data_size = std::min(max_fragment_payload_size, data.size()-i);
      ret.push_back(std::vector<uint8_t>(sizeof(FragmentHeader)+fragment_data_size));
      Fragment * fragment_packet = reinterpret_cast<Fragment*>(ret.back().data());
      fragment_packet->type = PacketType::Fragment;
      fragment_packet->packet_id = this_packet_id;
      fragment_packet->fragment_number = ret.size();
      memcpy(fragment_packet->fragment_data, &data.at(i), fragment_data_size);
    }
    for(auto & fragment_vector: ret)
      reinterpret_cast<Fragment*>(fragment_vector.data())->fragment_count = ret.size();
    if(ret.size() > std::numeric_limits<uint16_t>::max())
    {
      RCLCPP_WARN_STREAM(get_logger(), "Dropping " << ret.size() << " fragments, max frag count:" <<  std::numeric_limits<uint16_t>::max());
      return {};
    }
  }
  if(ret.empty())
    ret.push_back(data);
  return ret;
}

void UDPBridge::bridgeInfoCallback()
{
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    return;
  }

  sendBridgeInfo();
  sendConnectionRequests();
}

void UDPBridge::sendBridgeInfo()
{
  BridgeInfo bi;
  bi.stamp = get_clock()->now();
  bi.name = name_;
  // get_topic_names_and_types() is a node-graph query; safe to call without
  // any of our maps' mutexes.
  auto topic_names_and_types = get_topic_names_and_types();

  // Build the topics + remotes sections under scoped_lock so we get a
  // consistent snapshot. Connection statistics queries take their own
  // per-Connection mutex.
  {
    std::scoped_lock lock(subscribers_mutex_, remote_nodes_mutex_);
    for(auto topic: topic_names_and_types)
    {
      TopicInfo ti;
      ti.topic = topic.first;
      ti.datatype = topic.second.front();
      auto sub_it = subscribers_.find(topic.first);
      if(sub_it != subscribers_.end())
      {
        for(auto r: sub_it->second.remote_details)
        {
          TopicRemoteDetails trd;
          trd.remote = r.first;
          trd.destination_topic = r.second.destination_topic;
          for(auto c: r.second.connection_rates)
          {
            TopicRemoteConnectionDetails trcd;
            trcd.connection_id = c.first;
            trcd.period = c.second.period;
            trd.connections.push_back(trcd);
          }
          ti.remotes.push_back(trd);
        }
      }
      bi.topics.push_back(ti);
    }

    for(auto remote_node: remote_nodes_)
      if(remote_node.second)
      {
        udp_bridge::Remote remote;
        remote.name = remote_node.first;
        remote.topic_name = remote_node.second->topicName();
        for(auto connection: remote_node.second->connections())
          if(connection)
          {
            RemoteConnection rc;
            rc.connection_id = connection->id();
            rc.host = connection->host();
            rc.port = connection->port();
            rc.ip_address = connection->ip_address();
            rc.return_host = connection->returnHost();
            rc.return_port = connection->returnPort();
            rc.source_ip_address = connection->sourceIPAddress();
            rc.source_port = connection->sourcePort();
            rc.maximum_bytes_per_second = connection->rateLimit();
            auto receive_rates = connection->data_receive_rate(rclcpp::Time(bi.stamp).seconds());
            rc.received_bytes_per_second = receive_rates.first + receive_rates.second;
            rc.duplicate_bytes_per_second = receive_rates.second;
            rc.message = connection->data_sent_rate(bi.stamp, PacketSendCategory::message);
            rc.overhead = connection->data_sent_rate(bi.stamp, PacketSendCategory::overhead);
            rc.resend = connection->data_sent_rate(bi.stamp, PacketSendCategory::resend);
            remote.connections.push_back(rc);
          }
        bi.remotes.push_back(remote);
      }
  }

  bi.next_packet_number = next_packet_number_.load();
  // Reconstruct rclcpp::Time from the stored nanoseconds. The two atomic
  // loads aren't synchronized; minor diagnostic skew between
  // bi.next_packet_number and bi.last_packet_time is acceptable (both
  // are advisory display).
  bi.last_packet_time = rclcpp::Time(last_packet_number_assign_time_ns_.load());
  bi.local_port = port_;
  bi.maximum_packet_size = max_packet_size_;

  // publish + send run without our maps' mutexes.
  bridge_info_publisher_->publish(bi);

  send(bi, allRemotes(), true);
}

void UDPBridge::sendConnectionRequests()
{
  // Snapshot pending_connections_ entries we need under the lock, then
  // call send() (which acquires its own locks) without holding ours.
  std::vector<std::pair<uint64_t, ConnectionInternal>> snapshots;
  {
    std::lock_guard<std::mutex> lock(pending_connections_mutex_);
    snapshots.reserve(pending_connections_.size());
    for(auto& pending_connection: pending_connections_)
      snapshots.emplace_back(pending_connection.first, pending_connection.second.message);
  }
  for(auto& s: snapshots)
  {
    RemoteConnectionsList rcl;
    rcl[""].push_back(std::to_string(s.first));
    send(s.second, rcl, true);
  }
}

void UDPBridge::addRemote(
  std::shared_ptr<udp_bridge::AddRemote::Request> request,
  std::shared_ptr<udp_bridge::AddRemote::Response> response)
{
  (void)response;
  auto connection_id = request->connection_id;
  if(connection_id.empty())
    connection_id = "default";

  bool should_send_bridge_info = false;
  if(!request->name.empty())
  {
    {
      std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
      auto remote = remote_nodes_[request->name];
      if(!remote)
      {
        remote = std::make_shared<RemoteNode>(request->name, name_, *this);
        remote_nodes_[request->name] = remote;
      }

      uint16_t port = 4200;
      if (request->port != 0)
        port = request->port;

      auto connection = remote->connection(connection_id);
      if(!request->address.empty())
      {
        if(!connection)
        {
          connection = remote->newConnection(connection_id, request->address, port);
          remote->connections().push_back(connection);
        }
        else
          connection->setHostAndPort(request->address, port);
      }
      if(connection)
      {
        connection->setReturnHostAndPort(request->return_address, request->return_port);
        connection->setRateLimit(request->maximum_bytes_per_second);
        should_send_bridge_info = true;
      }
    }
    if(should_send_bridge_info)
      sendBridgeInfo();
  }

  if(request->name.empty() || request->return_maximum_bytes_per_second > 0) // we don't know the remote name, so send a connection request
  {
    ConnectionInternal connection_internal_message;
    {
      std::lock_guard<std::mutex> lock(pending_connections_mutex_);
      // fetch_add(1) gives a unique sequence_number under concurrent
      // service-handler invocations.
      auto sequence_number = next_connection_internal_message_sequence_number_.fetch_add(1);
      auto& connection_internal = pending_connections_[sequence_number];
      connection_internal.message.connection_id = connection_id;
      connection_internal.message.sequence_number = sequence_number;
      connection_internal.message.operation = ConnectionInternal::OPERATION_CONNECT;
      connection_internal.message.return_host = request->return_address;
      connection_internal.message.return_port = request->return_port;
      connection_internal.message.return_maximum_bytes_per_second = request->return_maximum_bytes_per_second;
      if(!request->address.empty())
      {
        connection_internal.connection = std::make_shared<Connection>(connection_id, request->address, request->port);
        connection_internal.connection->setRateLimit(request->maximum_bytes_per_second);
      }
      connection_internal_message = connection_internal.message;
    }
    RemoteConnectionsList rcl;
    if(request->name.empty())
      rcl[""].push_back(std::to_string(connection_internal_message.sequence_number));
    else
      rcl[request->name].push_back(connection_id);
    send(connection_internal_message, rcl, true);
  }
}

void UDPBridge::listRemotes(
  std::shared_ptr<udp_bridge::ListRemotes::Request> request,
  std::shared_ptr<udp_bridge::ListRemotes::Response> response)
{
  (void)request;
  auto now = get_clock()->now();
  std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
  for(auto remote: remote_nodes_)
    if(remote.second)
    {
      Remote r;
      r.name = remote.first;
      r.topic_name = remote.second->topicName();
      for(auto connection: remote.second->connections())
        if(connection)
        {
          RemoteConnection rc;
          rc.connection_id = connection->id();
          rc.host = connection->host();
          rc.port = connection->port();
          rc.ip_address = connection->ip_address();
          rc.return_host = connection->returnHost();
          rc.return_port = connection->returnPort();
          rc.source_ip_address = connection->sourceIPAddress();
          rc.source_port = connection->sourcePort();
          rc.maximum_bytes_per_second = connection->rateLimit();
          auto receive_rates = connection->data_receive_rate(now.seconds());
          rc.received_bytes_per_second = receive_rates.first + receive_rates.second;
          rc.duplicate_bytes_per_second = receive_rates.second;
          rc.message = connection->data_sent_rate(now, PacketSendCategory::message);
          rc.overhead = connection->data_sent_rate(now, PacketSendCategory::overhead);
          rc.resend = connection->data_sent_rate(now, PacketSendCategory::resend);
          r.connections.push_back(rc);
        }
      response->remotes.push_back(r);
    }
}

UDPBridge::RemoteConnectionsList UDPBridge::allRemotes() const
{
  RemoteConnectionsList ret;
  std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
  for(auto remote: remote_nodes_)
    if(remote.second)
      ret[remote.first];
  return ret;
}

void UDPBridge::diagnosticTick()
{
  if(!diagnostic_updater_)
    return;
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
    return;
  syncDiagnosticTasks();
  diagnostic_updater_->force_update();
}

void UDPBridge::syncDiagnosticTasks()
{
  if(!diagnostic_updater_)
    return;
  // Snapshot (remote_name, connection_id) pairs under the lock; the
  // diagnostic_updater_->add call captures by value so the registered task
  // does not need to outlive the lock.
  std::vector<std::pair<std::string, std::string>> remote_connection_pairs;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    for(auto& remote_entry: remote_nodes_)
    {
      if(!remote_entry.second)
        continue;
      for(auto& connection: remote_entry.second->connections())
      {
        if(!connection)
          continue;
        remote_connection_pairs.emplace_back(remote_entry.first, connection->id());
      }
    }
  }
  for(auto& pair: remote_connection_pairs)
  {
    const std::string& remote_name = pair.first;
    const std::string& connection_id = pair.second;
    std::string task_name = "udp_bridge " + name_ + ": " + remote_name + ": " + connection_id;
    if(diagnostic_task_names_.count(task_name))
      continue;
    diagnostic_updater_->add(task_name,
      [this, remote_name, connection_id](diagnostic_updater::DiagnosticStatusWrapper& stat)
      {
        diagnoseConnection(remote_name, connection_id, stat);
      });
    diagnostic_task_names_.insert(task_name);
  }
}

void UDPBridge::diagnoseConnection(const std::string& remote_name,
                                   const std::string& connection_id,
                                   diagnostic_updater::DiagnosticStatusWrapper& stat)
{
  std::shared_ptr<Connection> connection;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote_it = remote_nodes_.find(remote_name);
    if(remote_it == remote_nodes_.end() || !remote_it->second)
    {
      stat.summary(diagnostic_msgs::msg::DiagnosticStatus::STALE, "remote not registered");
      return;
    }
    connection = remote_it->second->connection(connection_id);
  }
  if(!connection)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::STALE, "connection not registered");
    return;
  }

  auto now_time = now();
  auto totals = connection->data_sent_rate(now_time);
  auto resends = connection->data_sent_rate(now_time, PacketSendCategory::resend);
  auto rx = connection->data_receive_rate(now_time.seconds());
  double last_rx = connection->last_receive_time();
  double rx_age = (last_rx > 0.0) ? (now_time.seconds() - last_rx) : -1.0;

  stat.add("host", connection->host());
  stat.add("port", static_cast<int>(connection->port()));
  stat.add("rate_limit_bytes_per_sec", static_cast<unsigned int>(connection->rateLimit()));
  stat.add("tx_ok_bytes_per_sec", totals.success_bytes_per_second);
  stat.add("tx_failed_bytes_per_sec", totals.failed_bytes_per_second);
  stat.add("tx_dropped_bytes_per_sec", totals.dropped_bytes_per_second);
  stat.add("tx_resend_bytes_per_sec", resends.success_bytes_per_second);
  stat.add("rx_bytes_per_sec", rx.first);
  stat.add("rx_duplicate_bytes_per_sec", rx.second);
  stat.add("last_rx_age_s", rx_age);

  constexpr double kStaleWarnSeconds = 5.0;
  constexpr double kStaleErrorSeconds = 10.0;

  std::string summary = "tx " + std::to_string(static_cast<uint64_t>(totals.success_bytes_per_second))
                      + " B/s, rx " + std::to_string(static_cast<uint64_t>(rx.first)) + " B/s";

  if(last_rx > 0.0 && rx_age > kStaleErrorSeconds)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::ERROR,
                 "no rx for " + std::to_string(static_cast<uint64_t>(rx_age)) + "s");
  }
  else if(totals.failed_bytes_per_second > 0 || totals.dropped_bytes_per_second > 0)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::WARN, "tx failures/drops");
  }
  else if(last_rx > 0.0 && rx_age > kStaleWarnSeconds)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::WARN,
                 "no rx for " + std::to_string(static_cast<uint64_t>(rx_age)) + "s");
  }
  else
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::OK, summary);
  }
}

// void UDPBridge::maximumPacketSizeCallback(const std_msgs::Int32::ConstPtr& msg)
// {
//   max_packet_size_ = msg->data;
// }


} // namespace udp_bridge

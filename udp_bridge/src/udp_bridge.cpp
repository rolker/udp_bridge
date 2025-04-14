#include "udp_bridge/udp_bridge.h"

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

  declare_parameter("port", m_port);
  m_port = get_parameter("port").as_int();
  RCLCPP_INFO_STREAM(get_logger(), "port: " << m_port); 

  declare_parameter("maximum_packet_size", m_max_packet_size);
  m_max_packet_size = get_parameter("maximum_packet_size").as_int();
  RCLCPP_INFO_STREAM(get_logger(), "maximum_packet_size: " << m_max_packet_size);

  //maximum_packet_size_subscriber_ = ros::NodeHandle("~").subscribe("maximum_packet_size", 1, &UDPBridge::maximumPacketSizeCallback, this);

  m_socket = socket(AF_INET, SOCK_DGRAM, 0);
  if(m_socket < 0)
  {
    RCLCPP_ERROR(get_logger(),"Failed creating socket");
    exit(1);
  }
    
  sockaddr_in bind_address; // address to listen on
  memset((char *)&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = htons(m_port);
  
  if(bind(m_socket, (sockaddr*)&bind_address, sizeof(bind_address)) < 0)
  {
    RCLCPP_ERROR(get_logger(), "Error binding socket");
    exit(1);
  }
  
  timeval socket_timeout;
  socket_timeout.tv_sec = 0;
  socket_timeout.tv_usec = 1000;
  if(setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) < 0)
  {
    RCLCPP_ERROR(get_logger(), "Error setting socket timeout");
    exit(1);
  }

  int buffer_size;
  unsigned int s = sizeof(buffer_size);
  getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "recv buffer size:" << buffer_size);
  buffer_size = 500000;
  setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "recv buffer size set to:" << buffer_size);
  buffer_size = 500000;
  setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "send buffer size set to:" << buffer_size);

  std::string node_name = get_name();
  subscribe_service_ = create_service<Subscribe>(node_name+"/remote_subscribe", std::bind(&UDPBridge::remoteSubscribe, this, _1, _2));
  advertise_service_ = create_service<Subscribe>(node_name+"/remote_advertise", std::bind(&UDPBridge::remoteAdvertise, this, _1, _2));

  add_remote_service_ = create_service<AddRemote>(node_name+"/add_remote", std::bind(&UDPBridge::addRemote, this, _1, _2));
  list_remotes_service_ = create_service<ListRemotes>(node_name+"/list_remotes", std::bind(&UDPBridge::listRemotes, this, _1, _2));
  
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
        declare_parameter(queue_size_param, 1);
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

        addSubscriberConnection( source, destination, queue_size, period, remote_info.name, connection.connection_id);
      }
    }
  }
  
  
  stats_report_timer_ = create_wall_timer(1s, std::bind(&UDPBridge::statsReportCallback, this));
  bridge_info_timer_ = create_wall_timer(2s, std::bind(&UDPBridge::bridgeInfoCallback, this));
  spin_timer_ = create_wall_timer(10ms, std::bind(&UDPBridge::spin_once, this));
  subscription_update_timer_ = create_wall_timer(1s, std::bind(&UDPBridge::updateLocalSubscriptions, this));

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
    p.fd = m_socket;
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
      getsockopt(m_socket,SOL_SOCKET,SO_RCVBUF,&buffer_size,&buffer_size_size);
      buffer.resize(buffer_size);
      receive_length = recvfrom(m_socket, &buffer.front(), buffer_size, 0, (sockaddr*)&remote_address, &remote_address_length);
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
  for(auto remote: remote_nodes_)
  {
    if(remote.second)
    {
      int discard_count = remote.second->defragmenter().cleanup(get_clock()->now() - rclcpp::Duration::from_seconds(5));
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

  RemoteConnectionsList destinations;
  // figure out which remote connection is due for a message to be sent.
  for(auto& remote_details: m_subscribers[topic_name].remote_details)
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
  }

  MessageSizeData size_data;
  size_data.message_size = message->size();
  size_data.timestamp = now;
  size_data.send_results[""][""];
  m_subscribers[topic_name].statistics.add(size_data);
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
  

  for(auto destination: destinations)
  {
    message_internal.destination_topic = m_subscribers[topic_name].remote_details[destination.first].destination_topic;
    RemoteConnectionsList connections;
    connections[destination.first] = destination.second;
    size_data = send(message_internal, connections, false);
    size_data.message_size = message->size();
    m_subscribers[topic_name].statistics.add(size_data);
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
  auto remote_node_iterator = remote_nodes_.find(source_info.node_name);
  if(remote_node_iterator != remote_nodes_.end())
    remote_node = remote_node_iterator->second;
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
  auto outer_message = deserialize<MessageInternal>(message);

  rclcpp::SerializedMessage serialized_message;
  serialized_message.reserve(outer_message.data.size());
  memcpy(serialized_message.get_rcl_serialized_message().buffer, outer_message.data.data(), outer_message.data.size());
  serialized_message.get_rcl_serialized_message().buffer_length = outer_message.data.size();
  
  // publish, but first advertise if publisher not present
  auto topic = outer_message.destination_topic;
  if(topic.empty())
    topic = outer_message.source_topic;
  if (m_publishers.find(topic) == m_publishers.end())
  {
    m_publishers[topic] = create_generic_publisher(topic, outer_message.datatype, rclcpp::QoS(1));
    sendBridgeInfo();
  }
  
  m_publishers[topic]->publish(serialized_message);
}

void UDPBridge::decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto bridge_info = deserialize<BridgeInfo>(message);

  auto remote_node = remote_nodes_[source_info.node_name];
  if(!remote_node)
  {
    remote_node = std::make_shared<RemoteNode>(source_info.node_name, name_, *this);
    remote_nodes_[source_info.node_name] = remote_node;
  }
  remote_node->update(bridge_info, source_info);

}

void UDPBridge::decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto rr = deserialize<ResendRequest>(message);

  auto remote_node = remote_nodes_.find(source_info.node_name);

  auto now = get_clock()->now();

  if(remote_node != remote_nodes_.end())
    for(auto connection: remote_node->second->connections())
      connection->resend_packets(rr.missing_packets, m_socket, now);
}

void UDPBridge::decodeTopicStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto topic_statistics = deserialize<TopicStatisticsArray>(message);

  auto remote = remote_nodes_.find(source_info.node_name);
  if(remote != remote_nodes_.end())
    remote->second->publishTopicStatistics(topic_statistics);
}


void UDPBridge::addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::string remote_node, std::string connection_id)
{
  if(!remote_node.empty())
  {
    // if(m_subscribers.find(source_topic) == m_subscribers.end())
    // {
    //   queue_size = std::max(queue_size, uint32_t(1));

    //   rclcpp::QoS qos(queue_size);
    //   qos.best_effort();

    //   auto info = get_publishers_info_by_topic(source_topic);
    //   if(!info.empty())
    //   {
    //     std::string topic_type = info.front().topic_type();

    //     auto cb = [this, source_topic, topic_type](std::shared_ptr<rclcpp::SerializedMessage> message)
    //     {
    //       this->callback(source_topic, topic_type, message);
    //     };
    //     rclcpp::SubscriptionOptions options;
    //     options.ignore_local_publications = true;
    //     m_subscribers[source_topic].subscription = create_generic_subscription(source_topic, topic_type, qos, cb, options);

    //   }
    // }
    m_subscribers[source_topic].queue_size = std::max(queue_size, uint32_t(1));
    m_subscribers[source_topic].remote_details[remote_node].destination_topic = destination_topic;
    m_subscribers[source_topic].remote_details[remote_node].connection_rates[connection_id].period = period;
    sendBridgeInfo();
  }
}

void UDPBridge::updateLocalSubscriptions()
{
  for (auto& subscriber: m_subscribers)
  {
    if(!subscriber.second.subscription)
    {
      const auto& source_topic = subscriber.first;
      auto info = get_publishers_info_by_topic(source_topic);
      if(!info.empty())
      {
        std::string topic_type = info.front().topic_type();

        auto cb = [this, source_topic, topic_type](std::shared_ptr<rclcpp::SerializedMessage> message)
        {
          this->callback(source_topic, topic_type, message);
        };

        rclcpp::QoS qos(subscriber.second.queue_size);
        qos.best_effort();
  
        rclcpp::SubscriptionOptions options;
        options.ignore_local_publications = true;
        m_subscribers[source_topic].subscription = create_generic_subscription(source_topic, topic_type, qos, cb, options);

      }
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

    

    auto remote_iterator = remote_nodes_.find(wrapped_packet->source_node);
    std::shared_ptr<RemoteNode> remote;
    if(remote_iterator != remote_nodes_.end())
      remote = remote_iterator->second;

    auto updated_source_info = source_info;
    updated_source_info.node_name = wrapped_packet->source_node;

    if(!remote)
    {
      if(wrapped_packet->source_node == name_)
      {
        RCLCPP_ERROR_STREAM(get_logger(), "Received a packet from a node with our name: " << name_ << " connection: " << wrapped_packet->connection_id << " host: " << source_info.host << " port: " << source_info.port);
        return;
      }
      else
      {
        remote = std::make_shared<RemoteNode>(wrapped_packet->source_node, name_, *this);
        remote_nodes_[wrapped_packet->source_node] = remote;
      }
    }
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
        send(connection_internal, source_info.node_name, true);
      }
      break;
    case ConnectionInternal::OPERATION_CONNECT_ACKNOWLEDGE:
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
          else
          {}
        }
        pending_connections_.erase(connection_internal.sequence_number);
      }
      break;
    default:
      RCLCPP_WARN_STREAM(get_logger(), "Unhandled ConnectionInternal operation type: " << connection_internal.operation);
  }

}


void UDPBridge::resendMissingPackets()
{
  for(auto remote: remote_nodes_)
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
    uint64_t packet_number = next_packet_number_++;
    last_packet_number_assign_time_ = now;
    wrapped_packets.push_back(WrappedPacket(packet_number, packet, now));
    size_data.sent_size += wrapped_packets.back().packet.size();
  }

  if(!wrapped_packets.empty())
  {
    size_data.timestamp = wrapped_packets.front().timestamp;
    for(auto remote: remotes)
    {
      std::vector<std::shared_ptr<Connection> > connections;
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
      for(auto connection: connections)
        if(connection)
        {
          auto result = connection->send(wrapped_packets, m_socket, name_, is_overhead, now);
          size_data.send_results[remote.first][connection->id()]=result;
        }
      
    }
  }

  return size_data;
}

void UDPBridge::cleanupSentPackets()
{
  auto now = get_clock()->now();
  if(now.nanoseconds() != 0)
  {
    auto old_enough = now - rclcpp::Duration::from_seconds(3.0);
    for(auto remote: remote_nodes_)
      if(remote.second)
        for(auto connection: remote.second->connections())
          if(connection)
            connection->cleanup_sent_packets(old_enough);
  }
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
  auto remote = remote_nodes_.find(request->remote);
  if(remote == remote_nodes_.end())
    return;
  auto connection = remote->second->connection(request->connection_id);
  if(!connection)
    return;

  addSubscriberConnection(request->source_topic, request->destination_topic, request->queue_size, request->period, request->remote, request->connection_id);
  sendBridgeInfo();
}

void UDPBridge::statsReportCallback()
{
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    return;
  }

  rclcpp::Time now = get_clock()->now();
  TopicStatisticsArray tsa;
  for(auto &subscriber: m_subscribers)
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

  topic_statistics_publisher_->publish(tsa);

  send(tsa, allRemotes(), true);
}

std::vector<std::vector<uint8_t> > UDPBridge::fragment(const std::vector<uint8_t>& data)
{
  std::vector<std::vector<uint8_t> > ret;

  unsigned long max_fragment_payload_size = m_max_packet_size-(sizeof(FragmentHeader)+sizeof(SequencedPacketHeader));
  if(data.size()+sizeof(SequencedPacketHeader) > m_max_packet_size)
  {
    for(unsigned long i = 0; i < data.size(); i += max_fragment_payload_size)
    {
      unsigned long fragment_data_size = std::min(max_fragment_payload_size, data.size()-i);
      ret.push_back(std::vector<uint8_t>(sizeof(FragmentHeader)+fragment_data_size));
      Fragment * fragment_packet = reinterpret_cast<Fragment*>(ret.back().data());
      fragment_packet->type = PacketType::Fragment;
      fragment_packet->packet_id = next_fragmented_packet_id_;
      fragment_packet->fragment_number = ret.size();
      memcpy(fragment_packet->fragment_data, &data.at(i), fragment_data_size);
    }
    for(auto & fragment_vector: ret)
      reinterpret_cast<Fragment*>(fragment_vector.data())->fragment_count = ret.size(); 
    next_fragmented_packet_id_++;
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
  for(auto topic: get_topic_names_and_types())
  {
    TopicInfo ti;
    ti.topic = topic.first;
    ti.datatype = topic.second.front();
    if (m_subscribers.find(topic.first) != m_subscribers.end())
    {
      for(auto r: m_subscribers[topic.first].remote_details)
      {
        TopicRemoteDetails trd;
        trd.remote = r.first;
        trd.destination_topic = r.second.destination_topic;
        for(auto c:r.second.connection_rates)
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

  bi.next_packet_number = next_packet_number_;
  bi.last_packet_time = last_packet_number_assign_time_;
  bi.local_port = m_port;
  bi.maximum_packet_size = m_max_packet_size;

  bridge_info_publisher_->publish(bi);

  send(bi, allRemotes(), true);
}

void UDPBridge::sendConnectionRequests()
{
  for(auto pending_connection: pending_connections_)
  {
    RemoteConnectionsList rcl;
    rcl[""].push_back(std::to_string(pending_connection.first));
    send(pending_connection.second.message, rcl, true);
  }
}

void UDPBridge::addRemote(
  std::shared_ptr<udp_bridge::AddRemote::Request> request,
  std::shared_ptr<udp_bridge::AddRemote::Response> response)
{
  auto connection_id = request->connection_id;
  if(connection_id.empty())
    connection_id = "default";

  if(!request->name.empty())
  {
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
      sendBridgeInfo();
    }
  }

  if(request->name.empty() || request->return_maximum_bytes_per_second > 0) // we don't know the remote name, so send a connection request
  {
    auto sequence_number = next_connection_internal_message_sequence_number_++;
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
    RemoteConnectionsList rcl;
    if(request->name.empty())
      rcl[""].push_back(std::to_string(sequence_number));
    else
      rcl[request->name].push_back(connection_id);
    send(connection_internal.message, rcl, true);
  }

}

void UDPBridge::listRemotes(
  std::shared_ptr<udp_bridge::ListRemotes::Request> request,
  std::shared_ptr<udp_bridge::ListRemotes::Response> response)
{
  auto now = get_clock()->now();
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
  for(auto remote: remote_nodes_)
    if(remote.second)
      ret[remote.first];
  return ret;
}

// void UDPBridge::maximumPacketSizeCallback(const std_msgs::Int32::ConstPtr& msg)
// {
//   m_max_packet_size = msg->data;
// }


} // namespace udp_bridge

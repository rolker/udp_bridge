#include "udp_bridge/udp_bridge.h"

#include "ros/ros.h"

#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <unordered_set>

#include "udp_bridge/RemoteSubscribeInternal.h"
#include "udp_bridge/MessageInternal.h"
#include "udp_bridge/TopicStatisticsArray.h"
#include "udp_bridge/BridgeInfo.h"
#include "udp_bridge/ResendRequest.h"
#include "udp_bridge/remote_node.h"
#include "udp_bridge/types.h"
#include "udp_bridge/utilities.h"

namespace udp_bridge
{

UDPBridge::UDPBridge()
{
  auto name = ros::this_node::getName();
  auto last_slash = name.rfind('/');
  if(last_slash != std::string::npos)
    name = name.substr(last_slash+1);
  setName(ros::param::param("~name", name));

  ROS_INFO_STREAM("name: " << name_);

  m_port = ros::param::param<int>("~port", m_port);
  ROS_INFO_STREAM("port: " << m_port); 

  m_max_packet_size = ros::param::param("~maxPacketSize", m_max_packet_size);
  ROS_INFO_STREAM("maxPacketSize: " << m_max_packet_size); 

  m_socket = socket(AF_INET, SOCK_DGRAM, 0);
  if(m_socket < 0)
  {
    ROS_ERROR("Failed creating socket");
    exit(1);
  }
    
  sockaddr_in bind_address; // address to listen on
  memset((char *)&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = htons(m_port);
  
  if(bind(m_socket, (sockaddr*)&bind_address, sizeof(bind_address)) < 0)
  {
    ROS_ERROR("Error binding socket");
    exit(1);
  }
  
  timeval socket_timeout;
  socket_timeout.tv_sec = 0;
  socket_timeout.tv_usec = 1000;
  if(setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) < 0)
  {
    ROS_ERROR("Error setting socket timeout");
    exit(1);
  }

  int buffer_size;
  unsigned int s = sizeof(buffer_size);
  getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  ROS_INFO_STREAM("recv buffer size:" << buffer_size);
  buffer_size = 500000;
  setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  ROS_INFO_STREAM("recv buffer size set to:" << buffer_size);
  buffer_size = 500000;
  setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, (void*)&buffer_size, &s);
  ROS_INFO_STREAM("send buffer size set to:" << buffer_size);

  ros::NodeHandle private_nodeHandle("~");
  subscribe_service_ = private_nodeHandle.advertiseService("remote_subscribe", &UDPBridge::remoteSubscribe, this);
  advertise_service_ = private_nodeHandle.advertiseService("remote_advertise", &UDPBridge::remoteAdvertise, this);

  add_remote_service_ = private_nodeHandle.advertiseService("add_remote", &UDPBridge::addRemote, this);
  list_remotes_service_ = private_nodeHandle.advertiseService("list_remotes", &UDPBridge::listRemotes, this);
  
  m_topicStatisticsPublisher = private_nodeHandle.advertise<TopicStatisticsArray>("topic_statistics",10,false);
  m_bridge_info_publisher = private_nodeHandle.advertise<BridgeInfo>("bridge_info", 1, true);
  
  XmlRpc::XmlRpcValue remotes_dict;
  if(private_nodeHandle.getParam("remotes",remotes_dict))
  {
    if(remotes_dict.getType() == XmlRpc::XmlRpcValue::TypeStruct)
    {
      for(auto remote:remotes_dict)
      {
        Remote remote_info;
        remote_info.name = remote.first;
        if(remote.second.hasMember("name"))
          remote_info.name = std::string(remote.second["name"]);

        remote_nodes_[remote_info.name] = std::make_shared<RemoteNode>(remote_info.name, name_);
        remote_nodes_[remote_info.name]->update(remote_info);

        if(remote.second.hasMember("connections"))
          for(auto connection_info: remote.second["connections"])
          {
            RemoteConnection connection;
            connection.connection_id = connection_info.first;
            if(connection_info.second.hasMember("host"))
              connection.host = std::string(connection_info.second["host"]);
            connection.port = 0;
            if(connection_info.second.hasMember("port"))
              connection.port = int(connection_info.second["port"]);
            if(connection_info.second.hasMember("returnHost"))
              connection.return_host = std::string(connection_info.second["returnHost"]);
            connection.return_port = 0;
            if(connection_info.second.hasMember("returnPort"))
              connection.return_port =int(connection_info.second["remotePort"]);
            if(connection_info.second.hasMember("maximumBytesPerSecond"))
            {
              auto mbps = int(connection_info.second["maximumBytesPerSecond"]);
              if(mbps > 0)
                connection.maximum_bytes_per_second = mbps;
            }
            remote_info.connections.push_back(connection);
            remote_nodes_[remote_info.name]->update(remote_info);

            if(connection_info.second.hasMember("topics"))
              for(auto topic: connection_info.second["topics"])
              {
                int queue_size = 1;
                if (topic.second.hasMember("queue_size"))
                  queue_size = topic.second["queue_size"];
                double period = 0.0;
                if (topic.second.hasMember("period"))
                  period = topic.second["period"];
                std::string source = topic.first;
                if (topic.second.hasMember("source"))
                  source = std::string(topic.second["source"]);
                source = ros::names::resolve(source);
                std::string destination = source;
                if (topic.second.hasMember("destination"))
                  destination = std::string(topic.second["destination"]);
                addSubscriberConnection(source, destination, 1, period, remote_info.name, connection.connection_id);
              }
          }
      }
    }
  }
  
  stats_report_timer_ = m_nodeHandle.createTimer(ros::Duration(1.0), &UDPBridge::statsReportCallback, this);
  bridge_info_timer_ = m_nodeHandle.createTimer(ros::Duration(2.0), &UDPBridge::bridgeInfoCallback, this);
}

void UDPBridge::setName(const std::string &name)
{
  name_ = name;
  if(name_.size() >= maximum_node_name_size-1)
  {
    name_ = name_.substr(0, maximum_node_name_size-1);
    ROS_WARN_STREAM("udp_bridge name truncated to " << name_);
  }
}

void UDPBridge::spin()
{
  
  while(ros::ok())
  {   
    sockaddr_in remote_address;
    socklen_t remote_address_length = sizeof(remote_address);
    while(true)
    {
      pollfd p;
      p.fd = m_socket;
      p.events = POLLIN;
      int ret = poll(&p, 1, 10);
      if(ret < 0)
        ROS_WARN_STREAM("poll error: " << int(errno) << " " << strerror(errno));
      if(ret > 0 && p.revents & POLLIN)
      {
        int receive_length = 0;
        std::vector<uint8_t> buffer;
        int buffer_size;
        unsigned int buffer_size_size = sizeof(buffer_size);
        getsockopt(m_socket,SOL_SOCKET,SO_RCVBUF,&buffer_size,&buffer_size_size);
        buffer.resize(buffer_size);
        receive_length = recvfrom(m_socket, &buffer.front(), buffer_size, 0, (sockaddr*)&remote_address, &remote_address_length);
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
        int discard_count = remote.second->defragmenter().cleanup(ros::Duration(5));
        if(discard_count)
          ROS_INFO_STREAM("Discarded " << discard_count << " incomplete packets from " << remote.first);
      }
    }
    cleanupSentPackets();
    resendMissingPackets();
    ros::spinOnce();
  }
}

void UDPBridge::callback(const ros::MessageEvent<topic_tools::ShapeShifter>& event)
{
  auto header = event.getConnectionHeader();
  ROS_DEBUG_STREAM_NAMED("send_messages", "Message from: " << header["callerid"]);
  ROS_DEBUG_STREAM_NAMED("send_messages", "  topic: " << header["topic"]);
  ROS_DEBUG_STREAM_NAMED("send_messages", "  type: " << header["type"]);
  // skip messages published by us to prevent loops
  if(event.getPublisherName() == ros::this_node::getName())
    return;
  auto now = event.getReceiptTime();
  ROS_DEBUG_STREAM_NAMED("send_messages",now);
  auto topic = header.find("topic");
  if(topic == header.end())
    return;

  auto topic_name = topic->second;
  auto msg = event.getConstMessage();

  RemoteConnectionsList destinations;
  // figure out which remote connection is due for a message to be sent.
  for(auto& remote_details: m_subscribers[topic_name].remote_details)
  {
    std::unordered_set<float> periods; // group the sending to connections with same period
    for(auto& connection_rate: remote_details.second.connection_rates)
      if(connection_rate.second.period >= 0)
        if(connection_rate.second.period == 0 || now-connection_rate.second.last_sent_time > ros::Duration(connection_rate.second.period) || periods.count(connection_rate.second.period) > 0)
        {
          destinations[remote_details.first].push_back(connection_rate.first);
          ROS_DEBUG_STREAM_NAMED("send_messages", "adding destination " << remote_details.first << ":" << connection_rate.first << " last sent time: " << connection_rate.second.last_sent_time);
          connection_rate.second.last_sent_time = now;
          if(periods.count(connection_rate.second.period) == 0)
            periods.insert(connection_rate.second.period);
        }
  }

  MessageSizeData size_data;
  size_data.message_size = msg->size();
  ROS_DEBUG_STREAM_NAMED("send_messages", "message size: " << size_data.message_size);
  size_data.timestamp = now;
  size_data.send_results[""][""];
  m_subscribers[topic_name].statistics.add(size_data);
  if (destinations.empty())
  {
    ROS_DEBUG_STREAM_NAMED("send_messages","No ready destination");
    return;
  }

  // First, serialize message in a MessageInternal message
  MessageInternal message;
  
  message.source_topic = topic_name;
  message.datatype = msg->getDataType();
  message.md5sum = msg->getMD5Sum();
  message.message_definition = msg->getMessageDefinition();

  
  message.data.resize(msg->size());
  
  ros::serialization::OStream stream(message.data.data(), msg->size());
  msg->write(stream);

  for(auto destination: destinations)
  {
    message.destination_topic = m_subscribers[topic_name].remote_details[destination.first].destination_topic;
    RemoteConnectionsList connections;
    connections[destination.first] = destination.second;
    size_data = send(message, connections, false);
    size_data.message_size = msg->size();
    m_subscribers[topic_name].statistics.add(size_data);
  }  
}

void UDPBridge::decode(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  if(message.empty())
  {
    ROS_DEBUG_STREAM("empty message from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
    return;
  }

  const Packet *packet = reinterpret_cast<const Packet*>(message.data());
  ROS_DEBUG_STREAM_NAMED("receive", "Received packet of type " << int(packet->type) << " and size " << message.size() << " from '" << source_info.node_name << "' (" << source_info.host << ":" << source_info.port << ")");
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
        if(remote_node->defragmenter().addFragment(message))
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
        ROS_WARN_STREAM("Unknown packet type: " << int(packet->type) << " from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
  }
}

void UDPBridge::decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet *packet = reinterpret_cast<const Packet*>(message.data());

  auto payload_size = message.size() - sizeof(PacketHeader);
  
  MessageInternal outer_message;

  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),payload_size);
  ros::serialization::Serializer<MessageInternal>::read(stream, outer_message);
  
  topic_tools::ShapeShifter ss;
  ss.morph(outer_message.md5sum, outer_message.datatype, outer_message.message_definition, "");
  ros::serialization::IStream message_stream(outer_message.data.data(), outer_message.data.size());
  ss.read(message_stream);
  ROS_DEBUG_STREAM_NAMED("receive", "decoded message of type: " << ss.getDataType() << " and size: " << ss.size());
  
  
  // publish, but first advertise if publisher not present
  auto topic = outer_message.destination_topic;
  if(topic.empty())
    topic = outer_message.source_topic;
  if (m_publishers.find(topic) == m_publishers.end())
  {
    m_publishers[topic] = ss.advertise(m_nodeHandle, topic, 1);
    sendBridgeInfo();
  }
  
  m_publishers[topic].publish(ss);
}

void UDPBridge::decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
  
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
  
  BridgeInfo bridge_info;
  try
  {
    ros::serialization::Serializer<BridgeInfo>::read(stream, bridge_info);
  }
  catch(const std::exception& e)
  {
    ROS_WARN_STREAM("Error decoding BridgeInfo: " << e.what());
    return;
  }

  auto remote_node = remote_nodes_[source_info.node_name];
  if(!remote_node)
  {
    remote_node = std::make_shared<RemoteNode>(source_info.node_name, name_);
    remote_nodes_[source_info.node_name] = remote_node;
  }
  remote_node->update(bridge_info, source_info);

}

void UDPBridge::decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
  
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
  
  ResendRequest rr;
  ros::serialization::Serializer<ResendRequest>::read(stream, rr);

  ROS_DEBUG_STREAM_NAMED("resend", "Request from " << source_info.node_name << " to resend " << rr.missing_packets.size() << " packets");

  auto remote_node = remote_nodes_.find(source_info.node_name);
  if(remote_node != remote_nodes_.end())
    for(auto connection: remote_node->second->connections())
      connection->resend_packets(rr.missing_packets, m_socket);
}

void UDPBridge::decodeTopicStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
  
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
  
  TopicStatisticsArray topic_statistics;
  ros::serialization::Serializer<TopicStatisticsArray>::read(stream, topic_statistics);

  auto remote = remote_nodes_.find(source_info.node_name);
  if(remote != remote_nodes_.end())
    remote->second->publishTopicStatistics(topic_statistics);
}


void UDPBridge::addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::string remote_node, std::string connection_id)
{
  if(!remote_node.empty())
  {
    if(m_subscribers.find(source_topic) == m_subscribers.end())
    {
      queue_size = std::max(queue_size, uint32_t(1));

      m_subscribers[source_topic].subscriber = m_nodeHandle.subscribe(source_topic, queue_size, &UDPBridge::callback, this);
    }
    m_subscribers[source_topic].remote_details[remote_node].destination_topic = destination_topic;
    m_subscribers[source_topic].remote_details[remote_node].connection_rates[connection_id].period = period;
    sendBridgeInfo();
  }
}

void UDPBridge::decodeSubscribeRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    RemoteSubscribeInternal remote_request;
    ros::serialization::Serializer<RemoteSubscribeInternal>::read(stream, remote_request);

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
        ROS_ERROR_STREAM("Received a packet from a node with our name: " << name_ << " connection: " << wrapped_packet->connection_id << " host: " << source_info.host << " port: " << source_info.port);
        return;
      }
      else
      {
        remote = std::make_shared<RemoteNode>(wrapped_packet->source_node, name_);
        remote_nodes_[wrapped_packet->source_node] = remote;
      }
    }
    try
    {
      decode(remote->unwrap(message, updated_source_info), updated_source_info);
    }
    catch(const std::exception& e)
    {
      ROS_ERROR_STREAM("problem decoding packet: " << e.what());
    }
  }
  else
    ROS_ERROR_STREAM("Incomplete wrapped packet of size: " << message.size());
}

void UDPBridge::decodeConnection(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
  ConnectionInternal connection_internal;
  ros::serialization::Serializer<ConnectionInternal>::read(stream, connection_internal);

  switch(connection_internal.operation)
  {
    case ConnectionInternal::OPERATION_CONNECT:
      {
        auto remote = remote_nodes_[source_info.node_name];
        if(!remote)
        {
          remote = std::make_shared<RemoteNode>(source_info.node_name, name_);
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
          remote = std::make_shared<RemoteNode>(source_info.node_name, name_);
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
          {
            connection->setRateLimit(pending_connection->rateLimit());
            connection->setReturnHostAndPort(pending_connection->returnHost(), pending_connection->returnPort());
          }
        }
        pending_connections_.erase(connection_internal.sequence_number);
      }
      break;
    default:
      ROS_WARN_STREAM("Unhandled ConnectionInternal operation type: " << connection_internal.operation);
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
        ROS_DEBUG_STREAM_NAMED("resend", "Sending a request to resend " << rr.missing_packets.size() << " packets");
        send(rr, remote.first, true);
      }
    }
}


template <typename MessageType> MessageSizeData UDPBridge::send(MessageType const &message, const RemoteConnectionsList& remotes, bool is_overhead)
{
  auto serial_size = ros::serialization::serializationLength(message);
  
  ROS_DEBUG_STREAM_NAMED("send_messages", "serial size: " << serial_size);
    
  std::vector<uint8_t> packet_data(sizeof(PacketHeader)+serial_size);
  Packet * packet = reinterpret_cast<Packet *>(packet_data.data());
  ros::serialization::OStream stream(packet->data, serial_size);
  ros::serialization::serialize(stream,message);
  packet->type = packetTypeOf(message);

  auto packet_size = packet_data.size();

  ROS_DEBUG_STREAM_NAMED("send_messages", "packet size: " << packet_size);

  packet_data = compress(packet_data);

  ROS_DEBUG_STREAM_NAMED("send_messages", "compressed packet size: " << packet_data.size());

  auto fragments = fragment(packet_data);
  ROS_DEBUG_STREAM_NAMED("send_messages", "fragment count: " << fragments.size());

  auto size_data = send(fragments, remotes, is_overhead);

  for(auto remote_result: size_data.send_results)
  {
    for(auto connection_result: remote_result.second)
    {
      switch(connection_result.second)
      {
      case SendResult::success:
        ROS_DEBUG_STREAM_NAMED("send_messages", remote_result.first << ":" << connection_result.first << " success");
        break;
      case SendResult::failed:
        ROS_DEBUG_STREAM_NAMED("send_messages", remote_result.first << ":" << connection_result.first << " failed");
        break;
      case SendResult::dropped:
        ROS_DEBUG_STREAM_NAMED("send_messages", remote_result.first << ":" << connection_result.first << " dropped");
        break;
      }
    }
  }

  size_data.message_size = serial_size;
  size_data.fragment_count = fragments.size();
  return size_data;
}

template <typename MessageType> MessageSizeData UDPBridge::send(MessageType const &message, const std::string& remote, bool is_overhead)
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
  
  ROS_DEBUG_STREAM_NAMED("send_messages", "begin packet number: " << next_packet_number_);

  for(auto packet: data)
  {
    uint64_t packet_number = next_packet_number_++;
    last_packet_number_assign_time_ = ros::Time::now();
    wrapped_packets.push_back(WrappedPacket(packet_number, packet));
    size_data.sent_size += wrapped_packets.back().packet.size();
  }

  ROS_DEBUG_STREAM_NAMED("send_messages", "end packet number: " << next_packet_number_);

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
          auto result = connection->send(wrapped_packets, m_socket, name_, is_overhead);
          size_data.send_results[remote.first][connection->id()]=result;
        }
      
    }
  }
  ROS_DEBUG_STREAM_NAMED("send_messages", "sent size: " << size_data.sent_size);

  return size_data;
}

void UDPBridge::cleanupSentPackets()
{
  auto now = ros::Time::now();
  if(now.isValid && !now.isZero())
  {
    auto old_enough = now - ros::Duration(3.0);
    for(auto remote: remote_nodes_)
      if(remote.second)
        for(auto connection: remote.second->connections())
          if(connection)
            connection->cleanup_sent_packets(old_enough);
  }
}


bool UDPBridge::remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
  ROS_INFO_STREAM("subscribe: remote: " << request.remote << ":" << " connection: " << request.connection_id << " source topic: " << request.source_topic << " destination topic: " << request.destination_topic);
  
  udp_bridge::RemoteSubscribeInternal remote_request;
  remote_request.source_topic = request.source_topic;
  remote_request.destination_topic = request.destination_topic;
  remote_request.queue_size = request.queue_size;
  remote_request.period = request.period;
  remote_request.connection_id = request.connection_id;

  auto ret = send(remote_request, request.remote, true);
  return ret.send_results[request.remote][request.connection_id] == SendResult::success;
}

bool UDPBridge::remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
  auto remote = remote_nodes_.find(request.remote);
  if(remote == remote_nodes_.end())
    return false;
  auto connection = remote->second->connection(request.connection_id);
  if(!connection)
    return false;

  addSubscriberConnection(request.source_topic, request.destination_topic, request.queue_size, request.period, request.remote, request.connection_id);
  sendBridgeInfo();
  return true;
}

void UDPBridge::statsReportCallback(ros::TimerEvent const &event)
{
  ros::Time now = ros::Time::now();
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

  m_topicStatisticsPublisher.publish(tsa);

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
      ROS_WARN_STREAM("Dropping " << ret.size() << " fragments, max frag count:" <<  std::numeric_limits<uint16_t>::max());
      return {};
    }
  }
  if(ret.empty())
    ret.push_back(data);
  return ret;
}

void UDPBridge::bridgeInfoCallback(ros::TimerEvent const &event)
{
  sendBridgeInfo();
  sendConnectionRequests();
}

void UDPBridge::sendBridgeInfo()
{
  BridgeInfo bi;
  bi.stamp = ros::Time::now();
  bi.name = name_;
  ros::master::V_TopicInfo topics;
  ros::master::getTopics(topics);
  for(auto mti: topics)
  {
    TopicInfo ti;
    ti.topic = mti.name;
    ti.datatype = mti.datatype;
    if (m_subscribers.find(mti.name) != m_subscribers.end())
    {
      for(auto r: m_subscribers[mti.name].remote_details)
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
          auto receive_rates = connection->data_receive_rate(bi.stamp.toSec());
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

  ROS_DEBUG_STREAM_NAMED("bridge_info","Publishing and sending BridgeInfo");

  m_bridge_info_publisher.publish(bi);

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

bool UDPBridge::addRemote(udp_bridge::AddRemote::Request &request, udp_bridge::AddRemote::Response &response)
{
  auto connection_id = request.connection_id;
  if(connection_id.empty())
    connection_id = "default";

  if(!request.name.empty())
  {
    auto remote = remote_nodes_[request.name];
    if(!remote)
    {
      remote = std::make_shared<RemoteNode>(request.name, name_);
      remote_nodes_[request.name] = remote;
    }

    uint16_t port = 4200;
    if (request.port != 0)
      port = request.port;

    auto connection = remote->connection(connection_id);
    if(!request.address.empty())
    {
      if(!connection)
      {
        connection = remote->newConnection(connection_id, request.address, port);
        remote->connections().push_back(connection);
      }
      else
        connection->setHostAndPort(request.address, port);
    }
    if(connection)
    {
      connection->setReturnHostAndPort(request.return_address, request.return_port);
      connection->setRateLimit(request.maximum_bytes_per_second);
      sendBridgeInfo();
    }
  }

  if(request.name.empty() || request.return_maximum_bytes_per_second > 0) // we don't know the remote name, so send a connection request
  {
    auto sequence_number = next_connection_internal_message_sequence_number_++;
    auto& connection_internal = pending_connections_[sequence_number];
    connection_internal.message.connection_id = connection_id;
    connection_internal.message.sequence_number = sequence_number;
    connection_internal.message.operation = ConnectionInternal::OPERATION_CONNECT;
    connection_internal.message.return_host = request.return_address;
    connection_internal.message.return_port = request.return_port;
    connection_internal.message.return_maximum_bytes_per_second = request.return_maximum_bytes_per_second;
    if(!request.address.empty())
    {
      connection_internal.connection = std::make_shared<Connection>(connection_id, request.address, request.port);
      connection_internal.connection->setRateLimit(request.maximum_bytes_per_second);
    }
    RemoteConnectionsList rcl;
    if(request.name.empty())
      rcl[""].push_back(std::to_string(sequence_number));
    else
      rcl[request.name].push_back(connection_id);
    send(connection_internal.message, rcl, true);
  }

  return true;
}

bool UDPBridge::listRemotes(udp_bridge::ListRemotes::Request &request, udp_bridge::ListRemotes::Response &response)
{
  auto now = ros::Time::now();
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
          auto receive_rates = connection->data_receive_rate(now.toSec());
          rc.received_bytes_per_second = receive_rates.first + receive_rates.second;
          rc.duplicate_bytes_per_second = receive_rates.second;
          rc.message = connection->data_sent_rate(now, PacketSendCategory::message);
          rc.overhead = connection->data_sent_rate(now, PacketSendCategory::overhead);
          rc.resend = connection->data_sent_rate(now, PacketSendCategory::resend);
          r.connections.push_back(rc);
        }
      response.remotes.push_back(r);
    }
  return true;
}

UDPBridge::RemoteConnectionsList UDPBridge::allRemotes() const
{
  RemoteConnectionsList ret;
  for(auto remote: remote_nodes_)
    if(remote.second)
      ret[remote.first];
  return ret;
}


} // namespace udp_bridge

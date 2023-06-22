#include "udp_bridge/udp_bridge.h"

#include "ros/ros.h"

#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>

#include "udp_bridge/RemoteSubscribeInternal.h"
#include "udp_bridge/MessageInternal.h"
#include "udp_bridge/ChannelStatisticsArray.h"
#include "udp_bridge/ChannelInfo.h"
#include "udp_bridge/BridgeInfo.h"
#include "udp_bridge/ResendRequest.h"
#include "udp_bridge/remote_node.h"
#include "udp_bridge/types.h"
#include "udp_bridge/utilities.h"

namespace udp_bridge
{

UDPBridge::UDPBridge()
{
  auto name = ros::this_node::getNamespace();
  if(!name.empty() && name[0] == '/')
    name = name.substr(1);
  name = topicString(name);
  if(name.empty())
    name = ros::this_node::getName();
  
  setName(ros::param::param("~name", name));

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
  ros::NodeHandle private_nodeHandle("~");
  ros::ServiceServer susbcribe_service = private_nodeHandle.advertiseService("remote_subscribe", &UDPBridge::remoteSubscribe, this);
  ros::ServiceServer advertise_service = private_nodeHandle.advertiseService("remote_advertise", &UDPBridge::remoteAdvertise, this);

  ros::ServiceServer add_remote_service = private_nodeHandle.advertiseService("add_remote", &UDPBridge::addRemote, this);
  ros::ServiceServer list_remotes_service = private_nodeHandle.advertiseService("list_remotes", &UDPBridge::listRemotes, this);
  
  m_channelInfoPublisher = private_nodeHandle.advertise<ChannelStatisticsArray>("channel_info",10,false);
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
  
  ros::Timer statsReportTimer = m_nodeHandle.createTimer(ros::Duration(1.0), &UDPBridge::statsReportCallback, this);
  ros::Timer bridgeInfoTimer = m_nodeHandle.createTimer(ros::Duration(5.0), &UDPBridge::bridgeInfoCallback, this);
  
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

void UDPBridge::callback(const topic_tools::ShapeShifter::ConstPtr& msg, const std::string &topic_name)
{
  ros::Time now = ros::Time::now();

  RemoteConnectionsList need_channel_info_list;
  for(auto remote_connection: m_subscribers[topic_name].remote_connections)
  {
    if(now - remote_connection.second.last_info_sent_time > ros::Duration(5.0))
    {
      need_channel_info_list[remote_connection.first.first].push_back(remote_connection.first.second);
      remote_connection.second.last_info_sent_time = now;
    }
  }

  if(!need_channel_info_list.empty())
  {
    ChannelInfo ci;
    ci.source_topic = topic_name;
    ci.datatype = msg->getDataType();
    ci.md5sum = msg->getMD5Sum();
    ci.message_definition = msg->getMessageDefinition();
    for(auto remote: need_channel_info_list)
      for(auto connection: remote.second)
      {
        ci.destination_topic = m_subscribers[topic_name].remote_connections[std::make_pair(remote.first, connection)].destination_topic;
        RemoteConnectionsList need_channel_info;
        need_channel_info[remote.first].push_back(connection);
        overhead_statistics_.add(send(ci, need_channel_info));
      }
  }

  RemoteConnectionsList destinations;

  for(auto remote_connection: m_subscribers[topic_name].remote_connections)
  {
    if(remote_connection.second.period >= 0)
      if(remote_connection.second.period == 0 || now-remote_connection.second.last_sent_time > ros::Duration(remote_connection.second.period))
      {
        destinations[remote_connection.first.first].push_back(remote_connection.first.second);
        remote_connection.second.last_sent_time = now;
      }
  }

  if (destinations.empty())
    return;

  local_topic_types_[topic_name] = msg->getDataType();

  // First, serialize message in a MessageInternal message
  MessageInternal message;
  
  message.source_topic = topic_name;

  
  message.data.resize(msg->size());
  
  ros::serialization::OStream stream(message.data.data(), msg->size());
  msg->write(stream);

  auto size_data = send(message, destinations);

  m_subscribers[topic_name].statistics.add(size_data);

}

void UDPBridge::decode(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  if(message.empty())
  {
    ROS_DEBUG_STREAM("empty message from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
    return;
  }

  const Packet *packet = reinterpret_cast<const Packet*>(message.data());
  ROS_DEBUG_STREAM("Received packet of type " << int(packet->type) << " and size " << message.size() << " from '" << source_info.node_name << "' (" << source_info.host << ":" << source_info.port << ")");
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
    case PacketType::ChannelInfo:
      decodeChannelInfo(message, source_info);
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
    case PacketType::ChannelStatistics:
        decodeChannelStatistics(message, source_info);
        break;
    case PacketType::WrappedPacket:
      unwrap(message, source_info);
      break;
    case PacketType::ResendRequest:
      decodeResendRequest(message, source_info);
      break;
    default:
        ROS_WARN_STREAM("Unknown packet type: " << int(packet->type) << " from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
  }
}

void UDPBridge::decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet *packet = reinterpret_cast<const Packet*>(message.data());

  auto remote_node = remote_nodes_[source_info.node_name];
  if (remote_node)
  {
    auto payload_size = message.size() - sizeof(PacketHeader);
    
    MessageInternal outer_message;

    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),payload_size);
    ros::serialization::Serializer<MessageInternal>::read(stream, outer_message);
    
    auto channel_info = remote_node->channelInfos().find(outer_message.source_topic);
    if(channel_info != remote_node->channelInfos().end())
    {    
      topic_tools::ShapeShifter ss;
      ss.morph(channel_info->second.md5sum, channel_info->second.datatype, channel_info->second.message_definition, "");
      ros::serialization::IStream message_stream(outer_message.data.data(), outer_message.data.size());
      ss.read(message_stream);
      ROS_DEBUG_STREAM("decoded message of type: " << ss.getDataType() << " and size: " << ss.size());
      
      
      // publish, but first advertise if publisher not present
      auto topic = channel_info->second.destination_topic;
      if(topic.empty())
        topic = channel_info->second.source_topic;
      if (m_publishers.find(topic) == m_publishers.end())
      {
        m_publishers[topic] = ss.advertise(m_nodeHandle, topic, 1);
        sendBridgeInfo();
      }
      
      m_publishers[topic].publish(ss);
    }
    else
      ROS_DEBUG_STREAM("no info yet for " << source_info.node_name << " " << outer_message.source_topic);
  }
}

void UDPBridge::decodeChannelInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
  
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
  
  ChannelInfo channel_info;
  ros::serialization::Serializer<ChannelInfo>::read(stream, channel_info);

  auto remote_node = remote_nodes_[source_info.node_name];
  if(remote_node)
  {
    remote_node->channelInfos()[channel_info.source_topic] = channel_info;
  }
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

  auto remote_node = remote_nodes_[source_info.node_name];
  if(remote_node)
  {
    for(auto packet_number: rr.missing_packets)
      for(auto connection: remote_node->connections())
      {
        auto packet_to_resend = connection->sentPackets().find(packet_number);
        if(packet_to_resend != connection->sentPackets().end())
          send(packet_to_resend->second.packet, connection->socket_address());
      }
  }
}

void UDPBridge::decodeChannelStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
  
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
  
  ChannelStatisticsArray channel_statistics;
  ros::serialization::Serializer<ChannelStatisticsArray>::read(stream, channel_statistics);

  auto remote = remote_nodes_[source_info.node_name];
  if(remote)
    remote->publishChannelStatistics(channel_statistics);
}


void UDPBridge::addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::string remote_node, std::string connection_id)
{
  if(!remote_node.empty())
  {
    if(m_subscribers.find(source_topic) == m_subscribers.end())
    {
      boost::function<void(const topic_tools::ShapeShifter::ConstPtr&) > cb = [source_topic,this](const topic_tools::ShapeShifter::ConstPtr& msg)
      {
        this->callback(msg, source_topic);
      };
      
      queue_size = std::max(queue_size, uint32_t(1));

      m_subscribers[source_topic].subscriber = m_nodeHandle.subscribe(source_topic, queue_size, cb);
    }
    m_subscribers[source_topic].remote_connections[std::make_pair(remote_node,connection_id)] = RemoteDetails(destination_topic, period);
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
      remote = std::make_shared<RemoteNode>(wrapped_packet->source_node, name_);
      remote_nodes_[wrapped_packet->source_node] = remote;
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

void UDPBridge::resendMissingPackets()
{
  ros::Time now = ros::Time::now();
  if(now.isValid() && !now.isZero())
  {
    ros::Time too_old = now - ros::Duration(5.0);
    for(auto remote: remote_nodes_)
      if(remote.second)
        remote.second->clearReceivedPacketTimesBefore(too_old);

    ros::Time can_resend_time = now - ros::Duration(0.2);

    for(auto remote: remote_nodes_)
      if(remote.second)
      {
        auto &rpts = remote.second->receivedPacketTimes();

        // trim the packet numbers that are lower than the largest packet number minus a threshold
        while(!rpts.empty() && rpts.rbegin()->first - rpts.begin()->first > 2500)
          rpts.erase(rpts.begin());

        std::vector<uint64_t> missing;
        if(!rpts.empty())
          for(auto i = rpts.begin()->first+1; i < rpts.rbegin()->first; i++)
            if(rpts.find(i) == rpts.end())
              missing.push_back(i);

        if(!missing.empty())
        {
          auto &rrts = remote.second->resendRequestTimes();
          ResendRequest rr;
          for(auto m: missing)
          {
            if(rrts[m] < can_resend_time)
            {
              rr.missing_packets.push_back(m);
              rrts[m] = now;
            }
          }
          if(!rr.missing_packets.empty())
            overhead_statistics_.add(send(rr, remote.first));
        }
      }
  }
}


template <typename MessageType> SizeData UDPBridge::send(MessageType const &message, const RemoteConnectionsList& remotes)
{
  auto serial_size = ros::serialization::serializationLength(message);
    
  std::vector<uint8_t> packet_data(sizeof(PacketHeader)+serial_size);
  Packet * packet = reinterpret_cast<Packet *>(packet_data.data());
  ros::serialization::OStream stream(packet->data, serial_size);
  ros::serialization::serialize(stream,message);
  packet->type = packetTypeOf(message);

  auto packet_size = packet_data.size();

  packet_data = compress(packet_data);

  auto fragments = fragment(packet_data);
  auto size_data = send(fragments, remotes);

  size_data.message_size = serial_size;
  size_data.packet_size = packet_size;
  size_data.compressed_packet_size = packet_data.size();
  size_data.fragment_count = fragments.size();
  return size_data;
}

template <typename MessageType> SizeData UDPBridge::send(MessageType const &message, const std::string& remote)
{
  RemoteConnectionsList rcl;
  rcl[remote];
  return send(message, rcl);
}


// wrap and send a series of packets that should be sent as a group, such as fragments.
template<> SizeData UDPBridge::send(const std::vector<std::vector<uint8_t> >& data, const RemoteConnectionsList& remotes)
{
  SizeData size_data;
  std::vector<WrappedPacket> wrapped_packets;
  for(auto packet: data)
  {
    uint64_t packet_number = next_packet_number_++;
    wrapped_packets.push_back(WrappedPacket(packet_number, packet));
    size_data.sent_size += wrapped_packets.back().packet.size();
  }

  if(!wrapped_packets.empty())
  {
    size_data.timestamp = wrapped_packets.front().timestamp;
    for(auto remote: remotes)
    {
      auto remote_node = remote_nodes_[remote.first];
      if(remote_node)
      {
        std::vector<std::shared_ptr<Connection> > connections;
        if(remote.second.empty())
          connections = remote_node->connections();
        else
          for(auto connection_id: remote.second)
            connections.push_back(remote_node->connection(connection_id));
        for(auto connection: connections)
          if(connection && !connection->host().empty())
          {
            SendResult result;
            result.sent_success = false;
            result.dropped = false;
            if(connection->can_send(size_data.sent_size, wrapped_packets.front().timestamp.toSec()))
            {
              result.sent_success = true;
              for(auto packet: wrapped_packets)
              {
                connection->sentPackets()[packet.packet_number] = WrappedPacket(packet, name_, connection->id());
                if(send(connection->sentPackets()[packet.packet_number].packet, connection->socket_address()) != packet.packet_size)
                  result.sent_success = false;
              }
            }
            else
              result.dropped = true;
            size_data.send_results[remote.first][connection->id()]=result;
          }

      }
    }
  }

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
          {
            std::vector<uint64_t> expired;
            for(auto sp: connection->sentPackets())
              if(sp.second.timestamp < old_enough)
                expired.push_back(sp.first);
            for(auto e: expired)
              connection->sentPackets().erase(e);
          }
  }
}

int UDPBridge::send(const std::vector<uint8_t> &data, const sockaddr_in* address)
{
  int bytes_sent = 0;
  try
  {
    int tries = 0;
    while (true)
    {
      pollfd p;
      p.fd = m_socket;
      p.events = POLLOUT;
      int ret = poll(&p, 1, 10);
      if(ret > 0 && p.revents & POLLOUT)
      {
        bytes_sent = sendto(m_socket, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(address), sizeof(*address));
        if(bytes_sent == -1)
          switch(errno)
          {
            case EAGAIN:
              tries+=1;
              break;
            case ECONNREFUSED:
              return bytes_sent;
            default:
              throw(ConnectionException(strerror(errno)));
          }
        if(bytes_sent < data.size())
          throw(ConnectionException("only "+std::to_string(bytes_sent) +" of " +std::to_string(data.size()) + " sent"));
        else
          break;
      }
      else
        tries+=1;

      if(tries >= 20)
        if(ret == 0)
          throw(ConnectionException("Timeout"));
        else
          throw(ConnectionException(std::to_string(errno) +": "+ strerror(errno)));
    }
    return bytes_sent;
  }
  catch(const ConnectionException& e)
  {
      ROS_WARN_STREAM("error sending data of size " << data.size() << ": " << e.getMessage());
  }
  return bytes_sent;
}


//template void UDPBridge::send(RemoteSubscribeInternal const &message, std::shared_ptr<Connection> connection, PacketType packetType);

bool UDPBridge::remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
  ROS_INFO_STREAM("subscribe: remote: " << request.remote << ":" << " connection: " << request.connection_id << " source topic: " << request.source_topic << " destination topic: " << request.destination_topic);
  
  udp_bridge::RemoteSubscribeInternal remote_request;
  remote_request.source_topic = request.source_topic;
  remote_request.destination_topic = request.destination_topic;
  remote_request.queue_size = request.queue_size;
  remote_request.period = request.period;
  remote_request.connection_id = request.connection_id;

  auto ret = send(remote_request, request.remote);
  overhead_statistics_.add(ret);
  return ret.send_results[request.remote][request.connection_id].sent_success;
}

bool UDPBridge::remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
  auto remote = remote_nodes_[request.remote];
  if(!remote)
    return false;
  auto connection = remote->connection(request.connection_id);
  if(!connection)
    return false;

  addSubscriberConnection(request.source_topic, request.destination_topic, request.queue_size, request.period, request.remote, request.connection_id);
  sendBridgeInfo();
  return true;
}

void UDPBridge::statsReportCallback(ros::TimerEvent const &event)
{
  ros::Time now = ros::Time::now();
  ChannelStatisticsArray csa;
  for(auto &subscriber: m_subscribers)
  {
    for(auto cs: subscriber.second.statistics.get())
    {
      cs.source_node = name_;
      cs.source_topic = subscriber.first;
      cs.destination_topic = subscriber.second.remote_connections[std::make_pair(cs.destination_node, cs.channel_id)].destination_topic;
      csa.channels.push_back(cs);
    }
  }

  for(auto cs: overhead_statistics_.get())
    csa.channels.push_back(cs);

  m_channelInfoPublisher.publish(csa);

  overhead_statistics_.add(send(csa, allRemotes()));
}

std::vector<std::vector<uint8_t> > UDPBridge::fragment(const std::vector<uint8_t>& data)
{
  std::vector<std::vector<uint8_t> > ret;

  unsigned long max_fragment_payload_size = m_max_packet_size-(sizeof(FragmentHeader)+sizeof(SequencedPacketHeader));
  if(data.size() > m_max_packet_size)
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
      for(auto r: m_subscribers[mti.name].remote_connections)
      {
        TopicRemoteDetails trd;
        trd.remote = r.first.first;
        trd.connection_id = r.first.second;
        trd.destination_topic = r.second.destination_topic;
        trd.period = r.second.period;
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
          rc.received_bytes_per_second = connection->data_receive_rate(bi.stamp.toSec());
          remote.connections.push_back(rc);
        }
      bi.remotes.push_back(remote);
    }


  m_bridge_info_publisher.publish(bi);

  overhead_statistics_.add(send(bi, allRemotes()));
}

bool UDPBridge::addRemote(udp_bridge::AddRemote::Request &request, udp_bridge::AddRemote::Response &response)
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

  auto connection = remote->connection(request.connection_id);
  if(!connection)
  {
    connection = remote->newConnection(request.connection_id, request.address, port);
    remote->connections().push_back(connection);
  }
  connection->setHostAndPort(request.address, request.port);
  connection->setReturnHostAndPort(request.return_address, request.return_port);

  sendBridgeInfo();
  return true;
}

bool UDPBridge::listRemotes(udp_bridge::ListRemotes::Request &request, udp_bridge::ListRemotes::Response &response)
{
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

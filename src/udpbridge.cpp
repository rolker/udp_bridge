#include "udp_bridge/udpbridge.h"

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

namespace udp_bridge
{

UDPBridge::UDPBridge()
{
    ros::param::param<int>("~port", m_port, m_port);
    ROS_INFO_STREAM("port: " << m_port); 

    ros::param::param<int>("~maxPacketSize", m_max_packet_size, m_max_packet_size);
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
              //std::cerr << remote.first << ": " << std::endl;
              std::string host = remote.second["host"];
              int port = remote.second["port"];
              std::string remote_name = remote.first;
              if(remote.second.hasMember("name"))
                  remote_name = std::string(remote.second["name"]);
              std::shared_ptr<Connection> connection = m_connectionManager.getConnection(host, port);
              connection->setLabel(remote_name);
              ROS_INFO_STREAM("remote: " << remote_name << " address: " << connection->ip_address_with_port());

              if(remote.second.hasMember("topics"))
                  for(auto topic: remote.second["topics"])
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
                      addSubscriberConnection(source, destination, 1, period, connection);
                  }
          }
      }
  }
  
  m_statsReportTimer = m_nodeHandle.createTimer(ros::Duration(1.0), &UDPBridge::statsReportCallback, this);
  m_bridgeInfoTimer = m_nodeHandle.createTimer(ros::Duration(5.0), &UDPBridge::bridgeInfoCallback, this);
  
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
          auto host = addressToDotted(remote_address);
          auto port = ntohs(remote_address.sin_port);
          auto c = m_connectionManager.getConnection(host, port);
          c->update_last_receive_time(ros::Time::now().toSec());

          buffer.resize(receive_length);
          //ROS_DEBUG_STREAM("received " << buffer.size() << " bytes");
          decode(buffer, c);
        }
      }
      else
        break;
    }
    int discard_count = m_defragmenter.cleanup(ros::Duration(10));
    if(discard_count)
        ROS_DEBUG_STREAM("Discarded " << discard_count << " incomplete packets");
    cleanupSentPackets();
    resendMissingPackets();
    ros::spinOnce();
  }
}

void UDPBridge::callback(const topic_tools::ShapeShifter::ConstPtr& msg, const std::string &topic_name)
{
    ros::Time now = ros::Time::now();

    if(now - m_channelInfoSentTimes[topic_name] > ros::Duration(5.0))
    {
      ChannelInfo ci;
      ci.source_topic = topic_name;
      ci.datatype = msg->getDataType();
      ci.md5sum = msg->getMD5Sum();
      ci.message_definition = msg->getMessageDefinition();
      m_channelInfoSentTimes[topic_name] = now;
      for (auto &remote:  m_subscribers[topic_name].remotes)
      {
        auto c = remote.second.connection.lock();
        if(c)
        {
          ci.destination_topic = remote.second.destination_topic;
          send(ci, c, PacketType::ChannelInfo);
        }
      }
    }

    bool dueToSend = false;
    for (auto &remote:  m_subscribers[topic_name].remotes)
      if (remote.second.period >= 0)
        if(remote.second.period == 0 ||  now-remote.second.last_sent_time > ros::Duration(remote.second.period))
        {
          dueToSend = true;
          break;
        }

    if (!dueToSend)
      return;

    ROS_DEBUG_STREAM("local msg on topic: " << topic_name << " type: " << msg->getDataType() << " size: " << msg->size());

    local_topic_types_[topic_name] = msg->getDataType();

    // First, serialize message in a MessageInternal message
    MessageInternal message;
    
    message.source_topic = topic_name;

    
    message.data.resize(msg->size());
    
    ros::serialization::OStream stream(message.data.data(), msg->size());
    msg->write(stream);

    // Next, wrap the MessageInternal into a packet

    auto msg_size = ros::serialization::serializationLength(message);

    std::vector<uint8_t> buffer(sizeof(PacketHeader)+msg_size);
    Packet * packet = reinterpret_cast<Packet*>(buffer.data());
    ros::serialization::OStream message_stream(packet->data, msg_size);
    ros::serialization::serialize(message_stream,message);

    // Now, compress the uncompressed packet
    std::shared_ptr<std::vector<uint8_t> > send_buffer = compress(buffer);
    auto send_size = send_buffer->size();
    std::vector<std::shared_ptr<std::vector<uint8_t> > > fragments = fragment(send_buffer);
    if(!fragments.empty())
    {
      send_size = 0;
      for(auto f: fragments)
        send_size+= f->size();
    }
    
    for (auto &remote:  m_subscribers[topic_name].remotes)
    {
        auto c = remote.second.connection.lock();
        if(c)
        {
            if (remote.second.period >= 0)
            {
                if(remote.second.period == 0 ||  now-remote.second.last_sent_time > ros::Duration(remote.second.period))
                {
                  if(c->can_send(send_size, now.toSec()))
                  {
                    bool success = true;
                    if(fragments.size())
                        for(const auto& f: fragments)
                        {
                            success = success && send(f,c);
                            usleep(500);
                        }
                    else
                        success = send(send_buffer,c);
                    
                    SizeData sd;
                    sd.sent_success = success;
                    sd.message_size = msg->size();
                    sd.packet_size = buffer.size();
                    sd.compressed_packet_size = send_buffer->size();
                    sd.timestamp = now;
                    remote.second.size_statistics.push_back(sd);
                    remote.second.last_sent_time = now;
                  }
                  else
                    ROS_DEBUG_STREAM("Dropped " << send_size << " bytes.");
                }
            }
        }
    }
}

void UDPBridge::decode(std::vector<uint8_t> const &message, const std::shared_ptr<Connection>& connection)
{
    const Packet *packet = reinterpret_cast<const Packet*>(message.data());
    ROS_DEBUG_STREAM("Received packet of type " << int(packet->type) << " and size " << message.size());
    switch(packet->type)
    {
        case PacketType::Data:
            decodeData(message, connection);
            break;
        case PacketType::Compressed:
            decode(uncompress(message), connection);
            break;
        case PacketType::SubscribeRequest:
            decodeSubscribeRequest(message, connection);
            break;
        case PacketType::ChannelInfo:
            decodeChannelInfo(message, connection);
            break;
        case PacketType::Fragment:
            if(m_defragmenter.addFragment(message, connection->label()))
                for(auto p: m_defragmenter.getPackets())
                    decode(p.first, m_connectionManager.getConnection(p.second));
            break;
        case PacketType::BridgeInfo:
            decodeBridgeInfo(message, connection);
            break;
        case PacketType::ChannelStatistics:
            decodeChannelStatistics(message, connection);
            break;
        case PacketType::WrappedPacket:
          unwrap(message, connection);
          break;
        case PacketType::ResendRequest:
          decodeResendRequest(message, connection);
          break;
        default:
            ROS_DEBUG_STREAM("Unkown packet type: " << int(packet->type));
    }
}

void UDPBridge::decodeData(std::vector<uint8_t> const &message, const std::shared_ptr<Connection>& connection)
{
    const Packet *packet = reinterpret_cast<const Packet*>(message.data());
    auto payload_size = message.size() - sizeof(PacketHeader);
    
    MessageInternal outer_message;

    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),payload_size);
    ros::serialization::Serializer<MessageInternal>::read(stream, outer_message);
    
    std::string info_label = connection->ip_address_with_port()+outer_message.source_topic;
    if(m_channelInfos.find(info_label) != m_channelInfos.end())
    {    
      topic_tools::ShapeShifter ss;
      ss.morph(m_channelInfos[info_label].md5sum, m_channelInfos[info_label].datatype, m_channelInfos[info_label].message_definition, "");
      ros::serialization::IStream message_stream(outer_message.data.data(), outer_message.data.size());
      ss.read(message_stream);
      ROS_DEBUG_STREAM("decoded message of type: " << ss.getDataType() << " and size: " << ss.size());
      
      
      // publish, but first advertise if publisher not present
      auto topic = m_channelInfos[info_label].destination_topic;
      if(topic.empty())
        topic =m_channelInfos[info_label].source_topic;
      if (m_publishers.find(topic) == m_publishers.end())
      {
        m_publishers[topic] = ss.advertise(m_nodeHandle, topic, 1);
        sendBridgeInfo();
      }
      
      m_publishers[topic].publish(ss);
    }
    else
      ROS_DEBUG_STREAM("no info yet for " << info_label);
    
}

void UDPBridge::decodeChannelInfo(std::vector<uint8_t> const &message, const std::shared_ptr<Connection>& connection)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    ChannelInfo channel_info;
    ros::serialization::Serializer<ChannelInfo>::read(stream, channel_info);
    
    m_channelInfos[connection->ip_address_with_port()+channel_info.source_topic] = channel_info;
}

void UDPBridge::decodeBridgeInfo(std::vector<uint8_t> const &message, const std::shared_ptr<Connection>& connection)
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
  
  //std::cerr << "BridgeInfo from " << remote_address << std::endl;
  auto label = connection->topicLabel();

  bool updated = false;

  if(m_bridge_info_publishers.find(label) == m_bridge_info_publishers.end())
  {
    ros::NodeHandle private_nodeHandle("~");
    m_bridge_info_publishers[label] = std::make_pair(private_nodeHandle.advertise<BridgeInfo>("remotes/"+label+"/bridge_info",1,true),ros::Time());
    updated = true;
  }
  m_bridge_info_publishers[label].first.publish(bridge_info);
  auto now = ros::Time::now();
  m_bridge_info_publishers[label].second = now;

  auto expire_time = now-ros::Duration(60.0);
  std::vector<std::string> expired;
  for(auto bip: m_bridge_info_publishers)
    if(bip.second.second < expire_time)
      expired.push_back(bip.first);

  for(auto e: expired)
  {
    ROS_INFO_STREAM(e << " bridge_info expired");
    m_bridge_info_publishers[e].first.shutdown();
    m_bridge_info_publishers.erase(e);
    updated = true;
  }
  if(updated)
    sendBridgeInfo();
}

void UDPBridge::decodeResendRequest(std::vector<uint8_t> const &message, const std::shared_ptr<Connection>& connection)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
  
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
  
  ResendRequest rr;
  ros::serialization::Serializer<ResendRequest>::read(stream, rr);

  auto label = connection->label();
  for(auto p: rr.missing_packets)
    if(wrapped_packets_[label].find(p) != wrapped_packets_[label].end())
      send(wrapped_packets_[label][p].packet, connection->socket_address());
}

void UDPBridge::decodeChannelStatistics(std::vector<uint8_t> const &message, const std::shared_ptr<Connection>& connection)
{
  const Packet* packet = reinterpret_cast<const Packet*>(message.data());
  
  ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
  
  ChannelStatisticsArray channel_statistics;
  ros::serialization::Serializer<ChannelStatisticsArray>::read(stream, channel_statistics);
  
  auto label = connection->topicLabel();

  bool updated = false;

  if(m_channel_statistics_publishers.find(label) == m_channel_statistics_publishers.end())
  {
    ros::NodeHandle private_nodeHandle("~");
    m_channel_statistics_publishers[label] = std::make_pair(private_nodeHandle.advertise<ChannelStatisticsArray>("remotes/"+label+"/channel_statistics",1,true),ros::Time());
    updated = true;
  }
  m_channel_statistics_publishers[label].first.publish(channel_statistics);
  auto now = ros::Time::now();
  m_channel_statistics_publishers[label].second = now;

  auto expire_time = now-ros::Duration(60.0);
  std::vector<std::string> expired;
  for(auto csp: m_channel_statistics_publishers)
    if(csp.second.second < expire_time)
      expired.push_back(csp.first);

  for(auto e: expired)
  {
    ROS_INFO_STREAM(e << " channel_statistics expired");
    m_channel_statistics_publishers[e].first.shutdown();
    m_channel_statistics_publishers.erase(e);
    updated = true;
  }
  if(updated)
    sendBridgeInfo();
}


const UDPBridge::SubscriberDetails *UDPBridge::addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::shared_ptr<Connection> connection)
{
    if(connection)
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
        m_subscribers[source_topic].remotes[connection->ip_address_with_port()] = RemoteDetails(destination_topic, period, connection);
        return &m_subscribers[source_topic];
    }
    sendBridgeInfo();
    return nullptr;
}

void UDPBridge::decodeSubscribeRequest(std::vector<uint8_t> const &message, const std::shared_ptr<Connection>& connection)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    RemoteSubscribeInternal remote_request;
    ros::serialization::Serializer<RemoteSubscribeInternal>::read(stream, remote_request);

    // std::string host = remote_address;
    // if(!remote_request.return_address.empty())
    //     host = remote_request.return_address;

    // std::shared_ptr<Connection> connection = m_connectionManager.getConnection(host, remote_request.port);
    auto subscription = addSubscriberConnection(remote_request.source_topic, remote_request.destination_topic, remote_request.queue_size, remote_request.period, connection);
}

void UDPBridge::unwrap(const std::vector<uint8_t>& message, const std::shared_ptr<Connection>& connection)
{
  received_packet_times_[connection->label()][reinterpret_cast<const SequencedPacket*>(message.data())->packet_number] = ros::Time::now();
  decode(std::vector<uint8_t>(message.begin()+sizeof(SequencedPacketHeader), message.end()), connection);
}

void UDPBridge::resendMissingPackets()
{
  ros::Time now = ros::Time::now();
  std::vector<std::pair<std::string, uint32_t> > expired;
  ros::Time too_old = now - ros::Duration(5.0);
  for(auto c: received_packet_times_)
  {
    auto p = c.second.rbegin();
    while(p != c.second.rend())
      if(p->second > too_old)
        p++;
      else
      {
        p++;
        while(p != c.second.rend())
        {
          expired.push_back(std::make_pair(c.first, p->first));
          p++;
        }
      }
  }
  for(auto e: expired)
  {
    received_packet_times_[e.first].erase(e.second);
    if(received_packet_times_[e.first].empty())
      received_packet_times_.erase(e.first);
  }

  expired.clear();
  for(auto c: resend_request_times_)
    for(auto p: c.second)
      if(p.second < too_old)
        expired.push_back(std::make_pair(c.first, p.first));

  for(auto e: expired)
  {
    resend_request_times_[e.first].erase(e.second);
    if(resend_request_times_[e.first].empty())
      resend_request_times_.erase(e.first);
  }

  ros::Time can_resend_time = now - ros::Duration(0.2);
  for(auto c: received_packet_times_)
    if(!c.second.empty())
    {
      auto connection = m_connectionManager.getConnection(c.first);
      std::vector<uint32_t> missing;
      for(uint32_t i = c.second.begin()->first+1; i < c.second.rbegin()->first; i++)
        if(c.second.find(i) == c.second.end())
          missing.push_back(i);
      if(!missing.empty())
      {
        ResendRequest rr;
        std::stringstream ss;
        for(auto m: missing)
        {
          ss << m << ",";
          if(connection && resend_request_times_[c.first][m] < can_resend_time)
          {
            rr.missing_packets.push_back(m);
            resend_request_times_[c.first][m] = now;
          }
        }
        if(!rr.missing_packets.empty())
          send(rr, connection, PacketType::ResendRequest);
        ROS_DEBUG_STREAM("missing for " << c.first << ": " << ss.str());
      }
    }

}

bool UDPBridge::send(std::shared_ptr<std::vector<uint8_t> > data, std::shared_ptr<Connection> connection)
{
  auto label = connection->label();
  uint32_t packet_number = next_packet_numbers_[label]++;
  WrappedPacket& wrapped_packet = wrapped_packets_[label][packet_number];
  wrapped_packet.packet_number = packet_number;
  wrapped_packet.timestamp = ros::Time::now();
  wrapped_packet.type = PacketType::WrappedPacket;
  wrapped_packet.packet.resize(sizeof(SequencedPacketHeader)+data->size());
  memcpy(wrapped_packet.packet.data(), &wrapped_packet, sizeof(SequencedPacketHeader));
  memcpy(reinterpret_cast<SequencedPacket*>(wrapped_packet.packet.data())->packet, data->data(), data->size());

  return send(wrapped_packet.packet, connection->socket_address());
}

void UDPBridge::cleanupSentPackets()
{
  std::vector<std::pair<std::string, uint32_t> > expired;
  ros::Time old_enough = ros::Time::now() - ros::Duration(5.0);
  for(auto c: wrapped_packets_)
    for(auto p: c.second)
      if(p.second.timestamp < old_enough)
        expired.push_back(std::make_pair(c.first, p.second.packet_number));
  
  for(auto e: expired)
  {
    wrapped_packets_[e.first].erase(e.second);
    if(wrapped_packets_[e.first].empty())
      wrapped_packets_.erase(e.first);
  }
}

bool UDPBridge::send(const std::vector<uint8_t> &data, const sockaddr_in& address)
{
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
        int e = sendto(m_socket, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if(e == -1)
          switch(errno)
          {
            case EAGAIN:
              tries+=1;
              break;
            case ECONNREFUSED:
              return false;
            default:
              throw(ConnectionException(strerror(errno)));
          }
        if(e < data.size())
          throw(ConnectionException("only "+std::to_string(e) +" of " +std::to_string(data.size()) + " sent"));
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
    return true;
  }
  catch(const ConnectionException& e)
  {
      ROS_WARN_STREAM("error sending data of size " << data.size() << ": " << e.getMessage());
  }
  return false;
}

template <typename MessageType> void UDPBridge::send(MessageType const &message, std::shared_ptr<Connection> connection, PacketType packetType)
{
    auto serial_size = ros::serialization::serializationLength(message);
    
    std::vector<uint8_t> packet_data(sizeof(PacketHeader)+serial_size);
    Packet * packet = reinterpret_cast<Packet *>(packet_data.data());
    ros::serialization::OStream stream(packet->data, serial_size);
    ros::serialization::serialize(stream,message);
    packet->type = packetType;
    
    auto compressed_packet_data = compress(packet_data);
    auto fragments = fragment(compressed_packet_data);
    
    bool success = true;
    if(fragments.size())
        for(const auto& f: fragments)
        {
            success = success && send(f,connection);
            usleep(1500);
        }
    else
        success = send(compressed_packet_data, connection);

    SizeData sd;
    sd.sent_success = success;
    sd.message_size = serial_size;
    sd.packet_size = packet_data.size();
    sd.compressed_packet_size = compressed_packet_data->size();
    sd.timestamp = ros::Time::now();
    m_overhead_stats[connection->label()].push_back(sd);
}

template void UDPBridge::send(RemoteSubscribeInternal const &message, std::shared_ptr<Connection> connection, PacketType packetType);

bool UDPBridge::remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
    ROS_INFO_STREAM("subscribe: remote: " << request.remote << ":" << " source topic: " << request.source_topic << " destination topic: " << request.destination_topic);
    
    udp_bridge::RemoteSubscribeInternal remote_request;
    remote_request.port = m_port;
    remote_request.source_topic = request.source_topic;
    remote_request.destination_topic = request.destination_topic;
    remote_request.queue_size = request.queue_size;
    remote_request.period = request.period;

    std::shared_ptr<Connection> connection = m_connectionManager.getConnection(request.remote);
    if(!connection)
        return false;
    remote_request.return_address = connection->returnHost();
    send(remote_request, connection, PacketType::SubscribeRequest);

    return true;
}

bool UDPBridge::remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
    std::shared_ptr<Connection> connection =  m_connectionManager.getConnection(request.remote);
    if(!connection)
        return false;

    auto subscription = addSubscriberConnection(request.source_topic, request.destination_topic, request.queue_size, request.period, connection);
    sendBridgeInfo();
    return true;
}

void UDPBridge::statsReportCallback(ros::TimerEvent const &event)
{
  ros::Time now = ros::Time::now();
  ChannelStatisticsArray csa;
  for(auto &subscriber: m_subscribers)
  {
    for(auto &remote: subscriber.second.remotes)
    {
      while (!remote.second.size_statistics.empty() && now - remote.second.size_statistics.front().timestamp > ros::Duration(10))
        remote.second.size_statistics.pop_front();
      
      {
        auto connection = remote.second.connection.lock();
        if(connection)
        {
          ChannelStatistics cs;
          cs.source_topic = subscriber.first;
          cs.destination_topic = remote.second.destination_topic;
          cs.remote = connection->label();
          
          int total_message_size = 0;
          int total_packet_size = 0;
          int total_compressed_packet_size = 0;
          int total_sent_success = 0;
          
          for(auto data: remote.second.size_statistics)
          {
            total_message_size += data.message_size;
            total_packet_size += data.packet_size;
            total_compressed_packet_size += data.compressed_packet_size;
            if(data.sent_success)
              total_sent_success++;
          }
          
          if(!remote.second.size_statistics.empty())
          {
            cs.message_average_size_bytes = total_message_size/float(remote.second.size_statistics.size());
            cs.packet_average_size_bytes = total_packet_size/float(remote.second.size_statistics.size());
            cs.compressed_average_size_bytes = total_compressed_packet_size /float(remote.second.size_statistics.size());
            cs.send_success_rate = total_sent_success/float(remote.second.size_statistics.size());
            double deltat = (remote.second.size_statistics.back().timestamp - remote.second.size_statistics.front().timestamp).toSec();
            if(deltat > 0.0)
            {
              cs.messages_per_second = (remote.second.size_statistics.size()-1)/deltat;
              cs.message_bytes_per_second = (total_message_size-remote.second.size_statistics.front().message_size)/deltat;
              cs.packet_bytes_per_second = (total_packet_size-remote.second.size_statistics.front().packet_size)/deltat;
              cs.compressed_bytes_per_second = (total_compressed_packet_size-remote.second.size_statistics.front().compressed_packet_size)/deltat;
            }
          }
          csa.channels.push_back(cs);
        }
      }
    }
  }
  for(auto& remote: m_overhead_stats)
  {
    while(!remote.second.empty() && now- remote.second.front().timestamp > ros::Duration(10))
      remote.second.pop_front();
    ChannelStatistics cs;
    cs.remote = remote.first;

    int total_message_size = 0;
    int total_packet_size = 0;
    int total_compressed_packet_size = 0;
    int total_sent_success = 0;
    
    for(auto data: remote.second)
    {
      total_message_size += data.message_size;
      total_packet_size += data.packet_size;
      total_compressed_packet_size += data.compressed_packet_size;
      if(data.sent_success)
        total_sent_success++;
    }
    
    if(!remote.second.empty())
    {
      cs.message_average_size_bytes = total_message_size/float(remote.second.size());
      cs.packet_average_size_bytes = total_packet_size/float(remote.second.size());
      cs.compressed_average_size_bytes = total_compressed_packet_size /float(remote.second.size());
      double deltat = (remote.second.back().timestamp - remote.second.front().timestamp).toSec();
      cs.send_success_rate = total_sent_success/float(remote.second.size());
      if(deltat > 0.0)
      {
        cs.messages_per_second = (remote.second.size()-1)/deltat;
        cs.message_bytes_per_second = (total_message_size-remote.second.front().message_size)/deltat;
        cs.packet_bytes_per_second = (total_packet_size-remote.second.front().packet_size)/deltat;
        cs.compressed_bytes_per_second = (total_compressed_packet_size-remote.second.front().compressed_packet_size)/deltat;
      }
    }
    csa.channels.push_back(cs);

  }
  m_channelInfoPublisher.publish(csa);
  for(auto c: m_connectionManager.connections())
  {
    if(c)
    {
      csa.remote_label = c->label();
      send(csa, c, PacketType::ChannelStatistics);
    }
  }


  // clean up stale stats
  std::vector<std::string> stale;
  for(auto remote: m_overhead_stats)
  {
    auto c = m_connectionManager.getConnection(remote.first);
    if(!c || c->label() != remote.first) // doesn't exist, or has been renamed
      stale.push_back(remote.first);
  }
  for(auto s: stale)
    m_overhead_stats.erase(s);
}

std::vector<std::shared_ptr<std::vector<uint8_t> > > UDPBridge::fragment(std::shared_ptr<std::vector<uint8_t> > data)
{
  std::vector<std::shared_ptr<std::vector<uint8_t> > > ret;

  unsigned long max_fragment_payload_size = m_max_packet_size-sizeof(FragmentHeader);
  if(data->size() > m_max_packet_size)
  {
    for(unsigned long i = 0; i < data->size(); i += max_fragment_payload_size)
    {
      unsigned long fragment_data_size = std::min(max_fragment_payload_size, data->size()-i);
      ret.push_back(std::make_shared<std::vector<uint8_t> >(sizeof(FragmentHeader)+fragment_data_size));
      Fragment * fragment_packet = reinterpret_cast<Fragment*>(ret.back()->data());
      fragment_packet->type = PacketType::Fragment;
      fragment_packet->packet_id = m_next_packet_id;
      fragment_packet->fragment_number = ret.size();
      memcpy(fragment_packet->fragment_data, &data->at(i), fragment_data_size);
    }
    for(auto & frag_vec: ret)
      reinterpret_cast<Fragment*>(frag_vec->data())->fragment_count = ret.size(); 
    m_next_packet_id++;
  }
  ROS_DEBUG_STREAM("fragment: data size: " << data->size() << " max size: " << m_max_packet_size << ": " << ret.size() << " fragments");
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
  bi.port = m_port;
  ros::master::V_TopicInfo topics;
  ros::master::getTopics(topics);
  for(auto mti: topics)
  {
    TopicInfo ti;
    ti.topic = mti.name;
    ti.datatype = mti.datatype;
    if (m_subscribers.find(mti.name) != m_subscribers.end())
    {
      for(auto r: m_subscribers[mti.name].remotes)
      {
        TopicRemoteDetails trd;
        auto c = r.second.connection.lock();
        if(c)
          trd.remote = c->label(true);
        trd.destination_topic = r.second.destination_topic;
        trd.period = r.second.period;
        ti.remotes.push_back(trd);
      }
    }
    bi.topics.push_back(ti);
  }

  for(auto r: m_connectionManager.connections())
  {
    if(r)
    {
        udp_bridge::Remote remote;
        remote.name = r->label();
        remote.host = r->host();
        remote.port = r->port();
        remote.ip_address = r->ip_address();
        remote.topic_label = r->topicLabel();
        bi.remotes.push_back(remote);
    }
  }

  m_bridge_info_publisher.publish(bi);
  for(auto c: m_connectionManager.connections())
  {
    if(c)
    {
      bi.remote_label = c->label();
      send(bi, c, PacketType::BridgeInfo);
    }
  }
}

bool UDPBridge::addRemote(udp_bridge::AddRemote::Request &request, udp_bridge::AddRemote::Response &response)
{
    uint16_t port = 4200;
    if (request.port != 0)
        port = request.port;
    std::shared_ptr<Connection> connection = m_connectionManager.getConnection(request.address, port, request.name);
    connection->setLabel(request.name);
    connection->setReturnHost(request.return_address);
    ROS_INFO_STREAM("remote: " << request.name << " address: " << connection->ip_address_with_port());
    sendBridgeInfo();
    return true;
}

bool UDPBridge::listRemotes(udp_bridge::ListRemotes::Request &request, udp_bridge::ListRemotes::Response &response)
{
    for(auto c: m_connectionManager.connections())
    {
      if(c)
      {  
        udp_bridge::Remote r;
        r.name = c->label();
        r.host = c->host();
        r.ip_address = c->ip_address();
        r.port = c->port();
        response.remotes.push_back(r);
      }
    }
    return true;
}

} // namespace udp_bridge

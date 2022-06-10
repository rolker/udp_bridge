#include "udp_bridge/udpbridge.h"

#include "ros/ros.h"

#include <sys/socket.h>
#include <netdb.h>

#include "udp_bridge/RemoteSubscribeInternal.h"
#include "udp_bridge/MessageInternal.h"
#include "udp_bridge/ChannelStatisticsArray.h"
#include "udp_bridge/ChannelInfo.h"
#include "udp_bridge/BridgeInfo.h"

namespace udp_bridge
{

UDPBridge::UDPBridge()
{
    ros::param::param<int>("~port", m_port, m_port);
    ROS_INFO_STREAM("port: " << m_port); 

    ros::param::param<int>("~maxPacketSize", m_max_packet_size, m_max_packet_size);
    ROS_INFO_STREAM("maxPacketSize: " << m_max_packet_size); 

    m_listen_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if(m_listen_socket < 0)
    {
        ROS_ERROR("Failed creating socket");
        exit(1);
    }
    
    sockaddr_in bind_address; // address to listen on
    memset((char *)&bind_address, 0, sizeof(bind_address));
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_address.sin_port = htons(m_port);
    
    if(bind(m_listen_socket, (sockaddr*)&bind_address, sizeof(bind_address)) < 0)
    {
        ROS_ERROR("Error binding socket");
        exit(1);
    }
    
    timeval socket_timeout;
    socket_timeout.tv_sec = 0;
    socket_timeout.tv_usec = 1000;
    if(setsockopt(m_listen_socket, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) < 0)
    {
        ROS_ERROR("Error setting socket timeout");
        exit(1);
    }

    int recv_buffer_size;
    unsigned int s = sizeof(recv_buffer_size);
    getsockopt(m_listen_socket, SOL_SOCKET, SO_RCVBUF, (void*)&recv_buffer_size, &s);
    ROS_INFO_STREAM("recv buffer size:" << recv_buffer_size);
    recv_buffer_size = 500000;
    setsockopt(m_listen_socket, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, sizeof(recv_buffer_size));
    getsockopt(m_listen_socket, SOL_SOCKET, SO_RCVBUF, (void*)&recv_buffer_size, &s);
    ROS_INFO_STREAM("recv buffer size set to:" << recv_buffer_size);
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
                ROS_INFO_STREAM("remote: " << remote_name << " send buffer size: " << connection->sendBufferSize());

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
    m_bridgeInfoTimer = m_nodeHandle.createTimer(ros::Duration(10.0), &UDPBridge::bridgeInfoCallback, this);
    
    while(ros::ok())
    {   
        sockaddr_in remote_address;
        socklen_t remote_address_length = sizeof(remote_address);
        int receive_length = 0;
        std::vector<uint8_t> buffer;
        int buffer_size;
        unsigned int buffer_size_size = sizeof(buffer_size);
        getsockopt(m_listen_socket,SOL_SOCKET,SO_RCVBUF,&buffer_size,&buffer_size_size);
        buffer.resize(buffer_size);
        receive_length = recvfrom(m_listen_socket, &buffer.front(), buffer_size, 0, (sockaddr*)&remote_address, &remote_address_length);
        if(receive_length > 0)
        {
            buffer.resize(receive_length);
            //ROS_DEBUG_STREAM("received " << buffer.size() << " bytes");
            
            decode(buffer, addressToDotted(remote_address));
        }
        int discard_count = m_defragmenter.cleanup(ros::Duration(10));
        if(discard_count)
            ROS_DEBUG_STREAM("Discarded " << discard_count << " incomplete packets");

        ros::spinOnce();
    }
}

void UDPBridge::callback(const topic_tools::ShapeShifter::ConstPtr& msg, const std::string &topic_name)
{
    ROS_DEBUG_STREAM("local msg on topic: " << topic_name << " type: " << msg->getDataType() << " size: " << msg->size());

    local_topic_types_[topic_name] = msg->getDataType();

    // First, serialize message in a MessageInternal message
    MessageInternal message;
    
    message.source_topic = topic_name;

    bool sendInfo = false;    
    
    ChannelInfo ci;
    
    ros::Time now = ros::Time::now();
    if(now - m_channelInfoSentTimes[topic_name] > ros::Duration(5.0))
    {
        sendInfo = true;
        ci.source_topic = topic_name;
        ci.datatype = msg->getDataType();
        ci.md5sum = msg->getMD5Sum();
        ci.message_definition = msg->getMessageDefinition();
        m_channelInfoSentTimes[topic_name] = now;
    }
    
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

    std::vector<std::shared_ptr<std::vector<uint8_t> > > fragments = fragment(send_buffer);
    
    for (auto &remote:  m_subscribers[topic_name].remotes)
    {
        auto c = remote.second.connection.lock();
        if(c)
        {
            if(sendInfo)
            {
                ci.destination_topic = remote.second.destination_topic;
                send(ci, c, PacketType::ChannelInfo);
            }
            
            if (remote.second.period >= 0)
            {
                if(remote.second.period == 0 ||  now-remote.second.last_sent_time > ros::Duration(remote.second.period))
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
                    sd.timestamp = ros::Time::now();
                    remote.second.size_statistics.push_back(sd);
                    remote.second.last_sent_time = now;
                }
            }
        }
    }
}

void UDPBridge::decode(std::vector<uint8_t> const &message, const std::string &remote_address)
{
    const Packet *packet = reinterpret_cast<const Packet*>(message.data());
    ROS_DEBUG_STREAM("Received packet of type " << int(packet->type) << " and size " << message.size());
    switch(packet->type)
    {
        case PacketType::Data:
            decodeData(message, remote_address);
            break;
        case PacketType::Compressed:
            decode(uncompress(message), remote_address);
            break;
        case PacketType::SubscribeRequest:
            decodeSubscribeRequest(message, remote_address);
            break;
        case PacketType::ChannelInfo:
            decodeChannelInfo(message, remote_address);
            break;
        case PacketType::Fragment:
            if(m_defragmenter.addFragment(message, remote_address))
                for(auto p: m_defragmenter.getPackets())
                    decode(p.first, p.second);
            break;
        case PacketType::BridgeInfo:
            decodeBridgeInfo(message, remote_address);
            break;
        default:
            ROS_DEBUG_STREAM("Unkown packet type: " << int(packet->type));
    }
}

void UDPBridge::decodeData(std::vector<uint8_t> const &message, const std::string &remote_address)
{
    const Packet *packet = reinterpret_cast<const Packet*>(message.data());
    auto payload_size = message.size() - sizeof(PacketHeader);
    
    MessageInternal outer_message;

    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),payload_size);
    ros::serialization::Serializer<MessageInternal>::read(stream, outer_message);
    
    std::string info_label = remote_address+outer_message.source_topic;
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
        m_publishers[topic] = ss.advertise(m_nodeHandle, topic, 1);
      
      m_publishers[topic].publish(ss);
    }
    else
      ROS_DEBUG_STREAM("no info yet for " << info_label);
    
}

void UDPBridge::decodeChannelInfo(std::vector<uint8_t> const &message, const std::string &remote_address)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    ChannelInfo channel_info;
    ros::serialization::Serializer<ChannelInfo>::read(stream, channel_info);
    
    m_channelInfos[remote_address+channel_info.source_topic] = channel_info;
}

void UDPBridge::decodeBridgeInfo(std::vector<uint8_t> const &message, const std::string &remote_address)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    BridgeInfo bridge_info;
    ros::serialization::Serializer<BridgeInfo>::read(stream, bridge_info);
    
    //std::cerr << "BridgeInfo from " << remote_address << std::endl;
    auto c = m_connectionManager.getConnection(remote_address, bridge_info.port);
    auto label = c->label();
    if(!std::isalpha(label[0]))
      label = "r"+label;
    for(int i = 0; i < label.size(); i++)
      if(!(std::isalnum(label[i]) || label[i] == '_' || label[i] == '/'))
        label[i] = '_';
    //std::cerr << "remote: " << label << std::endl;
    if(m_bridge_info_publishers.find(label) == m_bridge_info_publishers.end())
    {
      ros::NodeHandle private_nodeHandle("~");
      m_bridge_info_publishers[label] = private_nodeHandle.advertise<BridgeInfo>("remotes/"+label,1,true);
    }
    m_bridge_info_publishers[label].publish(bridge_info);
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
        m_subscribers[source_topic].remotes[connection->str()] = RemoteDetails(destination_topic, period, connection);
        return &m_subscribers[source_topic];
    }    
    return nullptr;
}

void UDPBridge::decodeSubscribeRequest(std::vector<uint8_t> const &message, const std::string &remote_address)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    RemoteSubscribeInternal remote_request;
    ros::serialization::Serializer<RemoteSubscribeInternal>::read(stream, remote_request);

    std::string host = remote_address;
    if(!remote_request.return_address.empty())
        host = remote_request.return_address;

    std::shared_ptr<Connection> connection = m_connectionManager.getConnection(host, remote_request.port);
    auto subscription = addSubscriberConnection(remote_request.source_topic, remote_request.destination_topic, remote_request.queue_size, remote_request.period, connection);
}

bool UDPBridge::send(std::shared_ptr<std::vector<uint8_t> > data, std::shared_ptr<Connection> connection)
{
    try
    {
        connection->send(data);
        return true;
    }
    catch(const ConnectionException& e)
    {
        ROS_WARN_STREAM("error sending data of size " << data->size() << ": " << e.getMessage());
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

    if(fragments.size())
        for(const auto& f: fragments)
        {
            send(f,connection);
            usleep(1500);
        }
    else
        send(compressed_packet_data, connection);
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
                    cs.destination_host = connection->str();
                    
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
                      double deltat = (remote.second.size_statistics.back().timestamp - remote.second.size_statistics.front().timestamp).toSec();
                      cs.messages_per_second = (remote.second.size_statistics.size()-1)/deltat;
                      cs.send_success_rate = total_sent_success/float(remote.second.size_statistics.size());
                      cs.message_bytes_per_second = (total_message_size-remote.second.size_statistics.front().message_size)/deltat;
                      cs.packet_bytes_per_second = (total_packet_size-remote.second.size_statistics.front().packet_size)/deltat;
                      cs.compressed_bytes_per_second = (total_compressed_packet_size-remote.second.size_statistics.front().compressed_packet_size)/deltat;
                    }
                    csa.channels.push_back(cs);
                }
                
            }
        }
    }
    m_channelInfoPublisher.publish(csa);
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
        trd.remote.connection = r.first;
        auto c = r.second.connection.lock();
        if(c)
          trd.remote.name = c->label(true);
        trd.destination_topic = r.second.destination_topic;
        trd.period = r.second.period;
        ti.remotes.push_back(trd);
      }
    }
    bi.topics.push_back(ti);
  }

  m_bridge_info_publisher.publish(bi);
  for(auto c: m_connectionManager.connections())
  {
    if(c)
      send(bi, c, PacketType::BridgeInfo);
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
    ROS_INFO_STREAM("remote: " << request.name << " send buffer size: " << connection->sendBufferSize());
    return true;
}

bool UDPBridge::listRemotes(udp_bridge::ListRemotes::Request &request, udp_bridge::ListRemotes::Response &response)
{
    for(auto c: m_connectionManager.connections())
    {
        udp_bridge::Remote r;
        r.name = c->label();
        r.connection = c->str();
        response.remotes.push_back(r);
    }
    return true;
}

} // namespace udp_bridge

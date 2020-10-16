#include "udp_bridge/udpbridge.h"

#include "ros/ros.h"
#include <sys/socket.h>
#include <netdb.h>
#include "udp_bridge/RemoteSubscribeInternal.h"
#include "udp_bridge/RemoteAdvertiseInternal.h"
#include "udp_bridge/MessageInternal.h"
#include "udp_bridge/ChannelStatisticsArray.h"

namespace udp_bridge
{

UDPBridge::UDPBridge()
{
    ros::param::param<int>("~port", m_port, m_port);
    ROS_INFO_STREAM("port: " << m_port); 

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
}

void UDPBridge::spin()
{
    ros::NodeHandle private_nodeHandle("~");
    ros::ServiceServer susbcribe_service = private_nodeHandle.advertiseService("remote_subscribe", &UDPBridge::remoteSubscribe, this);
    ros::ServiceServer advertise_service = private_nodeHandle.advertiseService("remote_advertise", &UDPBridge::remoteAdvertise, this);
    
    m_channelInfoPublisher = private_nodeHandle.advertise<ChannelStatisticsArray>("channel_info",10,false);
    
    XmlRpc::XmlRpcValue remotes_dict;
    if(private_nodeHandle.getParam("remotes",remotes_dict))
    {
        if(remotes_dict.getType() == XmlRpc::XmlRpcValue::TypeStruct)
        {
            for(auto remote:remotes_dict)
            {
                std::cerr << remote.first << ": " << std::endl;
                std::string host = remote.second["host"];
                int port = remote.second["port"];
                std::shared_ptr<Connection> connection = m_connectionManager.getConnection(host, port);
                for(int i = 0; i < remote.second["topics"].size(); i++)
                {
                    auto topic = remote.second["topics"][i];
                    addSubscriberConnection(topic["source"], topic["destination"], 1, connection);
                }
            }
        }
    }
    
    m_statsReportTimer = m_nodeHandle.createTimer(ros::Duration(1.0), &UDPBridge::statsReportCallback, this);
    
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
            ROS_DEBUG_STREAM("received " << buffer.size() << " bytes");
            
            decode(buffer, remote_address);
        }

        ros::spinOnce();
    }
}

void UDPBridge::callback(const topic_tools::ShapeShifter::ConstPtr& msg, const std::string &topic_name)
{
    ROS_DEBUG_STREAM("msg on topic: " << topic_name);
    ROS_DEBUG_STREAM("type: " << msg->getDataType());
    ROS_DEBUG_STREAM("MD5Sum: " << msg->getMD5Sum());
    ROS_DEBUG_STREAM("definition: " << msg->getMessageDefinition());
    ROS_DEBUG_STREAM("size: " << msg->size());
    
    MessageInternal message;
    
    message.source_topic = topic_name;
    message.datatype = msg->getDataType();
    message.md5sum = msg->getMD5Sum();
    message.message_definition = msg->getMessageDefinition();
    
    message.data.resize(msg->size());
    
    ros::serialization::OStream stream(message.data.data(), msg->size());
    msg->write(stream);
    
    for (auto &remote:  m_subscribers[topic_name].remotes)
    {
        auto c = remote.connection.lock();
        if(c)
        {
            message.destination_topic = remote.destination_topic;
        
            auto msg_size = ros::serialization::serializationLength(message);
        
            std::vector<uint8_t> buffer(sizeof(PacketHeader)+msg_size);
            Packet * packet = reinterpret_cast<Packet*>(buffer.data());
            ros::serialization::OStream message_stream(packet->data, msg_size);
            ros::serialization::serialize(message_stream,message);
        
            std::vector<uint8_t> send_buffer = compress(buffer);
            c->send(send_buffer);
            
            SizeData sd;
            sd.message_size = msg->size();
            sd.packet_size = buffer.size();
            sd.compressed_packet_size = send_buffer.size();
            sd.timestamp = ros::Time::now();
            remote.size_statistics.push_back(sd);
        }
    }
}

void UDPBridge::decode(std::vector<uint8_t> const &message, const sockaddr_in &remote_address)
{
    const Packet *packet = reinterpret_cast<const Packet*>(message.data());
    switch(packet->type)
    {
        case PacketType::Data:
            decodeData(message);
            break;
        case PacketType::Compressed:
            decode(uncompress(message), remote_address);
            break;
        case PacketType::SubscribeRequest:
            decodeSubscribeRequest(message, remote_address);
            break;
        case PacketType::AdvertiseRequest:
            decodeAdvertiseRequest(message);
            break;
    }
}

void UDPBridge::decodeData(std::vector<uint8_t> const &message)
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
    ROS_DEBUG_STREAM("type: " << ss.getDataType());
    ROS_DEBUG_STREAM("MD5Sum: " << ss.getMD5Sum());
    ROS_DEBUG_STREAM("definition: " << ss.getMessageDefinition());
    ROS_DEBUG_STREAM("size: " << ss.size());
    
    
    // publish, but first advertise if publisher not present
    if (m_publishers.find(outer_message.destination_topic) == m_publishers.end())
        m_publishers[outer_message.destination_topic] = ss.advertise(m_nodeHandle, outer_message.destination_topic, 1);
    
    m_publishers[outer_message.destination_topic].publish(ss);
    
}

const UDPBridge::SubscriberDetails *UDPBridge::addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, std::shared_ptr<Connection> connection)
{
    if(connection)
    {
        if(m_subscribers.find(source_topic) == m_subscribers.end())
        {
            boost::function<void(const topic_tools::ShapeShifter::ConstPtr&) > cb = [source_topic,this](const topic_tools::ShapeShifter::ConstPtr& msg)
            {
                this->callback(msg, source_topic);
            };

            m_subscribers[source_topic].subscriber = m_nodeHandle.subscribe(source_topic, queue_size, cb);
        }
        m_subscribers[source_topic].remotes.push_back(RemoteDetails(destination_topic,connection));
        return &m_subscribers[source_topic];
    }    
    return nullptr;
}

void UDPBridge::decodeSubscribeRequest(std::vector<uint8_t> const &message, const sockaddr_in &remote_address)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    RemoteSubscribeInternal remote_request;
    ros::serialization::Serializer<RemoteSubscribeInternal>::read(stream, remote_request);

    std::shared_ptr<Connection> connection = m_connectionManager.getConnection(addressToDotted(remote_address), remote_request.port);
    auto subscription = addSubscriberConnection(remote_request.source_topic, remote_request.destination_topic, remote_request.queue_size, connection);
    
}

void UDPBridge::decodeAdvertiseRequest(std::vector<uint8_t> const &message)
{
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    
    ros::serialization::IStream stream(const_cast<uint8_t*>(packet->data),message.size()-sizeof(PacketHeader));
    
    RemoteAdvertiseInternal remote_request;
    ros::serialization::Serializer<RemoteAdvertiseInternal>::read(stream, remote_request);
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
    
    connection->send(compressed_packet_data);
}

template void UDPBridge::send(RemoteSubscribeInternal const &message, std::shared_ptr<Connection> connection, PacketType packetType);

bool UDPBridge::remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
    ROS_INFO_STREAM("subscribe: remote: " << request.remote_host << ":" << request.remote_port << " remote topic: " << request.remote_topic << " local topic: " << request.local_topic);
    
    udp_bridge::RemoteSubscribeInternal remote_request;
    remote_request.port = m_port;
    remote_request.source_topic = request.remote_topic;
    remote_request.destination_topic = request.local_topic;
    remote_request.queue_size = request.queue_size;

    std::shared_ptr<Connection> connection = m_connectionManager.getConnection(request.remote_host, request.remote_port);
    send(remote_request, connection, PacketType::SubscribeRequest);

    return true;
}

bool UDPBridge::remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response)
{
    std::shared_ptr<Connection> connection = m_connectionManager.getConnection(request.remote_host, request.remote_port);
    auto subscription = addSubscriberConnection(request.local_topic, request.remote_topic, request.queue_size, connection);

    RemoteAdvertiseInternal remote_request;
    remote_request.destination_topic = request.remote_topic;
    remote_request.source_topic = request.local_topic;
    remote_request.queue_size = request.queue_size;
    
    send(remote_request, connection, PacketType::AdvertiseRequest);

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
            while (!remote.size_statistics.empty() && now - remote.size_statistics.front().timestamp > ros::Duration(10))
                remote.size_statistics.pop_front();
            
            if(remote.size_statistics.size() > 1 && remote.size_statistics.back().timestamp - remote.size_statistics.front().timestamp >= ros::Duration(1.0))
            {
                auto connection = remote.connection.lock();
                if(connection)
                {
                    ChannelStatistics cs;
                    cs.source_topic = subscriber.first;
                    cs.destination_topic = remote.destination_topic;
                    cs.destination_host = connection->str();
                    
                    int total_message_size = 0;
                    int total_packet_size = 0;
                    int total_compressed_packet_size = 0;
                    
                    for(auto data: remote.size_statistics)
                    {
                        total_message_size += data.message_size;
                        total_packet_size += data.packet_size;
                        total_compressed_packet_size += data.compressed_packet_size;
                    }
                    
                    cs.message_average_size_bytes = total_message_size/float(remote.size_statistics.size());
                    cs.packet_average_size_bytes = total_packet_size/float(remote.size_statistics.size());
                    cs.compressed_average_size_bytes = total_compressed_packet_size /float(remote.size_statistics.size());
                    double deltat = (remote.size_statistics.back().timestamp - remote.size_statistics.front().timestamp).toSec();
                    cs.messages_per_second = (remote.size_statistics.size()-1)/deltat;
                    cs.message_bytes_per_second = (total_message_size-remote.size_statistics.front().message_size)/deltat;
                    cs.packet_bytes_per_second = (total_packet_size-remote.size_statistics.front().packet_size)/deltat;
                    cs.compressed_bytes_per_second = (total_compressed_packet_size-remote.size_statistics.front().compressed_packet_size)/deltat;
                    csa.channels.push_back(cs);
                }
                
            }
        }
    }
    m_channelInfoPublisher.publish(csa);
}

} // namespace udp_bridge

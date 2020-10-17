#ifndef UDP_BRIDGE_UDPBRIDGE_H
#define UDP_BRIDGE_UDPBRIDGE_H

#include <topic_tools/shape_shifter.h>
#include "udp_bridge/Subscribe.h"
#include "udp_bridge/ChannelInfo.h"
#include <netinet/in.h>
#include "connection.h"
#include "packet.h"
#include <deque>

namespace udp_bridge
{

class UDPBridge
{
public:
    UDPBridge();

    void spin();
    
private:
    /// Callback method for locally subscribed topics.
    /// ShapeShifter is used to be agnostic of message type at compile time.
    void callback(const topic_tools::ShapeShifter::ConstPtr& msg, const std::string &topic_name);
    
    /// Decodes outer layer of packets recieved over the UDP link and calls appropriate handlers base
    /// on packet type.
    void decode(std::vector<uint8_t> const &message, const sockaddr_in &remote_address);
    
    /// Decodes data from a remote subscription recieved over the UDP link.
    void decodeData(std::vector<uint8_t> const &message, const sockaddr_in &remote_address);
    
    /// Decodes metadata used to decode remote messages.
    void decodeChannelInfo(std::vector<uint8_t> const &message, const sockaddr_in &remote_address);
    
    /// Decodes a request from a remote node to subscribe to a local topic.
    void decodeSubscribeRequest(std::vector<uint8_t> const &message, const sockaddr_in &remote_address);
    
    /// Decodes a request from a remote node to advertise a topic locally.
    void decodeAdvertiseRequest(std::vector<uint8_t> const &message);
    
    /// Service handler for local request to subscribe to a remote topic.
    bool remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

    /// Service handler to advertise on a remote node a local topic.
    bool remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

    template <typename MessageType> void send(MessageType const &message, std::shared_ptr<Connection> connection, PacketType packetType);
    
    /// Timer callback where data rate stats are reported
    void statsReportCallback(const ros::TimerEvent&);
    

    int m_listen_socket;
    int m_port {4200};
    ros::NodeHandle m_nodeHandle;
    
    ros::Timer m_statsReportTimer;
    ros::Publisher m_channelInfoPublisher;
    
    struct SizeData
    {
        int message_size;
        int packet_size;
        int compressed_packet_size;
        ros::Time timestamp;
    };

    struct RemoteDetails
    {
        RemoteDetails(std::string const &destination_topic, std::weak_ptr<Connection> connection):destination_topic(destination_topic),connection(connection)
        {}
        
        std::string destination_topic;
        std::weak_ptr<Connection> connection;
        
        std::deque<SizeData> size_statistics;
    };
    
    struct SubscriberDetails
    {
        ros::Subscriber subscriber;
        std::vector<RemoteDetails> remotes;
    };
    
    std::map<std::string,SubscriberDetails> m_subscribers;
    std::map<std::string,ros::Publisher> m_publishers;
    std::map<std::string,ros::Time> m_channelInfoSentTimes;
    std::map<std::string,ChannelInfo> m_channelInfos;
    
    SubscriberDetails const *addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, std::shared_ptr<Connection> connection);
    
    ConnectionManager m_connectionManager;
};

} // namespace udp_bridge

#endif

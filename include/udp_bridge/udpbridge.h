#ifndef UDP_BRIDGE_UDPBRIDGE_H
#define UDP_BRIDGE_UDPBRIDGE_H

#include <topic_tools/shape_shifter.h>
#include "udp_bridge/Subscribe.h"
#include "udp_bridge/AddRemote.h"
#include "udp_bridge/ListRemotes.h"
#include "udp_bridge/ChannelInfo.h"
#include <netinet/in.h>
#include "connection.h"
#include "packet.h"
#include "defragmenter.h"
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
    
    /// Decodes outer layer of packets received over the UDP link and calls appropriate handlers base
    /// on packet type.
    void decode(std::vector<uint8_t> const &message, const std::string &remote_address);
    
    /// Decodes data from a remote subscription recieved over the UDP link.
    void decodeData(std::vector<uint8_t> const &message, const std::string &remote_address);
    
    /// Decodes metadata used to decode remote messages.
    void decodeChannelInfo(std::vector<uint8_t> const &message, const std::string &remote_address);

    /// Decodes topic info from remote.
    void decodeBridgeInfo(std::vector<uint8_t> const &message, const std::string &remote_address);
    
    /// Decodes a request from a remote node to subscribe to a local topic.
    void decodeSubscribeRequest(std::vector<uint8_t> const &message, const std::string &remote_address);
    
    /// Service handler for local request to subscribe to a remote topic.
    bool remoteSubscribe(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

    /// Service handler to advertise on a remote node a local topic.
    bool remoteAdvertise(udp_bridge::Subscribe::Request &request, udp_bridge::Subscribe::Response &response);

    /// Service handler to add a named remote
    bool addRemote(udp_bridge::AddRemote::Request &request, udp_bridge::AddRemote::Response &response);

    /// Service handler to list named remotes
    bool listRemotes(udp_bridge::ListRemotes::Request &request, udp_bridge::ListRemotes::Response &response);

    template <typename MessageType> void send(MessageType const &message, std::shared_ptr<Connection> connection, PacketType packetType);

    /// Sends the raw data to the connection. Returns true on success.
    bool send(std::shared_ptr<std::vector<uint8_t> > data, std::shared_ptr<Connection> connection);
    
    /// Timer callback where data rate stats are reported
    void statsReportCallback(const ros::TimerEvent&);


    /// Timer callback where info on subscibed channels are reported
    void bridgeInfoCallback(const ros::TimerEvent&);

    /// Splits up a packet, if necessary.
    /// Returns an empty vector if fragmentation is not necessary.
    std::vector<std::shared_ptr<std::vector<uint8_t> > > fragment(std::shared_ptr<std::vector<uint8_t> > data);    

    int m_listen_socket;
    int m_port {4200};
    int m_max_packet_size {65500};
    uint32_t m_next_packet_id {0};

    Defragmenter m_defragmenter;

    ros::NodeHandle m_nodeHandle;
    
    ros::Timer m_statsReportTimer;
    ros::Publisher m_channelInfoPublisher;

    ros::Timer m_bridgeInfoTimer;
    ros::Publisher m_bridge_info_publisher;
        
    struct SizeData
    {
        bool sent_success;
        int message_size;
        int packet_size;
        int compressed_packet_size;
        ros::Time timestamp;
    };

    struct RemoteDetails
    {
        RemoteDetails(){}
        RemoteDetails(std::string const &destination_topic, float period, std::weak_ptr<Connection> connection):destination_topic(destination_topic),period(period),connection(connection)
        {}
        
        std::string destination_topic;
        float period;
        ros::Time last_sent_time;
        
        std::weak_ptr<Connection> connection;
        
        std::deque<SizeData> size_statistics;
    };
    
    struct SubscriberDetails
    {
        ros::Subscriber subscriber;
        std::map<std::string,RemoteDetails> remotes;
    };
    
    std::map<std::string,SubscriberDetails> m_subscribers;
    std::map<std::string,ros::Publisher> m_publishers;
    std::map<std::string,ros::Time> m_channelInfoSentTimes;
    std::map<std::string,ChannelInfo> m_channelInfos;
    std::map<std::string, std::string> local_topic_types_;
    std::map<std::string,ros::Publisher> m_bridge_info_publishers;
    
    SubscriberDetails const *addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::shared_ptr<Connection> connection);
    
    ConnectionManager m_connectionManager;
};

} // namespace udp_bridge

#endif

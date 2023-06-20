#ifndef UDP_BRIDGE_STRUCTS_H
#define UDP_BRIDGE_STRUCTS_H

#include "udp_bridge/statistics.h"

namespace udp_bridge
{


struct RemoteDetails
{
  RemoteDetails()=default;
  //RemoteDetails(std::string const &destination_topic, float period, std::string remote_node, std::string channel_id):destination_topic(destination_topic),period(period),remote_node(remote_node), channel_id(channel_id)
  RemoteDetails(std::string const &destination_topic, float period):destination_topic(destination_topic),period(period)
  {}
      
  std::string destination_topic;
  float period;
  ros::Time last_sent_time;
  ros::Time last_info_sent_time;
      
  //std::string remote_node;
  //std::string channel_id;
      
};
    
struct SubscriberDetails
{
  ros::Subscriber subscriber;
  std::map<std::pair<std::string, std::string>, RemoteDetails> remote_connections;
  Statistics statistics;
};

// Name and address/port from which a message was received
struct SourceInfo
{
  std::string node_name;
  std::string host;
  uint16_t port = 0;
};

} // namespace udp_bridge

#endif
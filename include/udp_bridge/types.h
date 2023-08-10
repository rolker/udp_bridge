#ifndef UDP_BRIDGE_TYPES_H
#define UDP_BRIDGE_TYPES_H

#include "udp_bridge/statistics.h"

namespace udp_bridge
{

struct ConnectionRateInfo
{
  float period;
  ros::Time last_sent_time;
};

struct RemoteDetails
{
  RemoteDetails()=default;
  //RemoteDetails(std::string const &destination_topic, float period):destination_topic(destination_topic),period(period)
  //{}
      
  std::string destination_topic;
  std::map<std::string, ConnectionRateInfo> connection_rates;

};
    
struct SubscriberDetails
{
  ros::Subscriber subscriber;
  std::map<std::string, RemoteDetails> remote_details;
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
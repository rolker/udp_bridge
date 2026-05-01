#ifndef UDP_BRIDGE_TYPES_H
#define UDP_BRIDGE_TYPES_H

#include "udp_bridge/statistics.h"
#include "rclcpp/generic_subscription.hpp"

namespace udp_bridge
{

struct ConnectionRateInfo
{
  float period;
  rclcpp::Time last_sent_time;
};

struct RemoteDetails
{
  RemoteDetails()=default;
  //RemoteDetails(std::string const &destination_topic, float period):destination_topic(destination_topic),period(period)
  //{}

  std::string destination_topic;
  std::map<std::string, ConnectionRateInfo> connection_rates;

  // Per-topic QoS that the receiver should apply to the destination
  // publisher. See udp_bridge/doc/qos_design.md. Empty strings and 0
  // mean "use the package default" (best_available / volatile /
  // KEEP_LAST(1)).
  std::string reliability;
  std::string durability;
  uint32_t history_depth = 0;
};

struct SubscriberDetails
{
  rclcpp::GenericSubscription::SharedPtr subscription;
  uint32_t queue_size;
  std::map<std::string, RemoteDetails> remote_details;
  MessageStatistics statistics;

  // Source-side subscription QoS (the bridge's local subscriber). Set from
  // per-topic config. Reliability stays best_effort by convention (accepts
  // any source publisher); only durability is configurable, so the bridge
  // can subscribe transient_local when latching semantics matter.
  std::string durability;
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

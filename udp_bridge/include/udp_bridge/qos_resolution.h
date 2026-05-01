#ifndef UDP_BRIDGE_QOS_RESOLUTION_H
#define UDP_BRIDGE_QOS_RESOLUTION_H

#include <string>
#include <cstdint>
#include "rclcpp/qos.hpp"

namespace udp_bridge
{

/// Resolve the package's QoS contract from the three string/int fields
/// communicated end-to-end via MessageInternal (and stashed locally on
/// RemoteDetails / SubscriberDetails). See udp_bridge/doc/qos_design.md.
///
/// Defaults (used when the corresponding input is empty / 0):
///   reliability = best_available
///   durability  = volatile
///   history_depth = 1
///
/// Unrecognized strings fall through to defaults rather than erroring,
/// to preserve graceful interop with other versions of the package.
inline rclcpp::QoS resolveDestinationPublisherQos(
  const std::string& reliability,
  const std::string& durability,
  uint32_t history_depth)
{
  uint32_t depth = (history_depth == 0) ? 1u : history_depth;
  rclcpp::QoS qos(depth);
  qos.keep_last(depth);

  if(reliability == "reliable")
    qos.reliable();
  else if(reliability == "best_effort")
    qos.best_effort();
  else
    qos.reliability_best_available();

  if(durability == "transient_local")
    qos.transient_local();
  else
    qos.durability_volatile();

  return qos;
}

/// Resolve the source-side subscription's durability. Reliability is
/// always best_effort on the subscriber side (accepts any publisher).
/// Default is volatile; set to "transient_local" to receive latched
/// values when the bridge joins late.
inline rclcpp::QoS resolveSourceSubscriptionQos(
  const std::string& durability,
  uint32_t queue_size)
{
  uint32_t depth = (queue_size == 0) ? 1u : queue_size;
  rclcpp::QoS qos(depth);
  qos.keep_last(depth);
  qos.best_effort();
  if(durability == "transient_local")
    qos.transient_local();
  else
    qos.durability_volatile();
  return qos;
}

} // namespace udp_bridge

#endif

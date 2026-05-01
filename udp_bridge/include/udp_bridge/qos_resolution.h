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
///   reliability = reliable  (see note below)
///   durability  = volatile
///   history_depth = 1
///
/// Unrecognized strings fall through to defaults rather than erroring,
/// to preserve graceful interop with other versions of the package.
///
/// Reliability default is RELIABLE rather than BEST_AVAILABLE because
/// rmw_zenoh_cpp 0.2.9 fails to encode BEST_AVAILABLE into its liveliness
/// keyexpr ("Error setting QoS values from strings: unordered_map::at"
/// at liveliness_utils.cpp:340), which leaves the destination publisher
/// invisible to graph-discovery clients (e.g. CAMP's NavSource gates on
/// get_topic_names_and_types). RELIABLE matches both RELIABLE and
/// BEST_EFFORT subscribers; the "transparency lie" warned about in
/// qos_design.md is accepted here for operational graph visibility. Set
/// `reliability: best_available` per-topic to opt back in once the rmw
/// is fixed.
inline rclcpp::QoS resolveDestinationPublisherQos(
  const std::string& reliability,
  const std::string& durability,
  uint32_t history_depth)
{
  uint32_t depth = (history_depth == 0) ? 1u : history_depth;
  rclcpp::QoS qos(depth);
  qos.keep_last(depth);

  if(reliability == "best_effort")
    qos.best_effort();
  else if(reliability == "best_available")
    qos.reliability_best_available();
  else
    qos.reliable();

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

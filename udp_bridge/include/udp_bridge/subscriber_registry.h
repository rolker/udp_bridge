#ifndef UDP_BRIDGE_SUBSCRIBER_REGISTRY_H
#define UDP_BRIDGE_SUBSCRIBER_REGISTRY_H

#include <map>
#include <string>
#include <utility>

#include "rclcpp/generic_subscription.hpp"
#include "udp_bridge/types.h"

namespace udp_bridge
{

// Result of removeSubscriberConnectionFrom. `changed` is true when something was
// actually removed (so the caller republishes bridge info). `expired_subscription`
// holds the local generic subscription moved out of the registry when a source
// topic lost its last remote — the caller resets it OFF the registry lock as
// hygiene (subscription teardown does rmw work better kept off the hot mutex; an
// in-flight forwarding callback keeps its own strong ref via the executor, so
// the reset is safe regardless). Pure data + map logic, no node / clock / mutex
// here.
struct SubscriberRemoval
{
  bool changed = false;
  rclcpp::GenericSubscription::SharedPtr expired_subscription;
};

// Remove the (remote_node, connection_id) forwarding for source_topic from a
// subscriber registry. Drops the remote when it has no connections left, and the
// whole source entry — moving out its subscription for off-lock teardown — when
// it has no remotes left. Removing something absent is a no-op (changed=false),
// so repeated calls are idempotent.
inline SubscriberRemoval removeSubscriberConnectionFrom(
  std::map<std::string, SubscriberDetails>& subscribers,
  const std::string& source_topic,
  const std::string& remote_node,
  const std::string& connection_id)
{
  SubscriberRemoval result;
  auto sub_it = subscribers.find(source_topic);
  if(sub_it == subscribers.end())
    return result;
  auto rd_it = sub_it->second.remote_details.find(remote_node);
  if(rd_it == sub_it->second.remote_details.end())
    return result;
  if(rd_it->second.connection_rates.erase(connection_id) == 0)
    return result;
  result.changed = true;
  if(rd_it->second.connection_rates.empty())
    sub_it->second.remote_details.erase(rd_it);
  if(sub_it->second.remote_details.empty())
  {
    result.expired_subscription = std::move(sub_it->second.subscription);
    subscribers.erase(sub_it);
  }
  return result;
}

}  // namespace udp_bridge

#endif

#ifndef UDP_BRIDGE_RELAY_SEND_H
#define UDP_BRIDGE_RELAY_SEND_H

#include <exception>
#include <map>
#include <string>
#include <vector>

#include "udp_bridge/connection.h"
#include "udp_bridge/destination_selection.h"
#include "udp_bridge/relay_item.h"

namespace udp_bridge
{

/// Send one relayed message to each selected destination in turn, isolating
/// per-destination failures (issue #51).
///
/// Two properties, both of which the single try/catch this replaced got
/// wrong:
///
///   - **ConnectionException is caught.** It has no base class
///     (`connection.h`), so `catch(const std::exception&)` does NOT catch it
///     — and it is what `Connection::send()` throws on the ordinary Timeout
///     path, i.e. the failure the relay diagnostics exist to surface. An
///     uncaught throw would unwind into RelayQueue's last-resort `catch(...)`,
///     which has no logger, and every relay send failure would be silent.
///   - **One bad destination does not cancel the others.** A stalled or
///     misconfigured remote must not abort the fan-out to the remaining
///     remotes; the hub keeps serving the operators that are still healthy.
///
/// @param destinations the (remote -> connection ids) selection to send to
/// @param send_one called once per destination as
///        `send_one(remote_name, connection_ids)`; may throw
/// @param on_error called as `on_error(remote_name, message)` for a
///        destination whose send threw. Must not throw.
template<typename SendFn, typename ErrorFn>
void sendToEachDestination(const SelectedConnections& destinations,
                           SendFn&& send_one,
                           ErrorFn&& on_error)
{
  for(const auto& destination: destinations)
  {
    try
    {
      send_one(destination.first, destination.second);
    }
    catch(const ConnectionException& e)
    {
      // No base class: this must be caught before (and separately from)
      // std::exception, or not at all.
      on_error(destination.first, e.getMessage());
    }
    catch(const std::exception& e)
    {
      on_error(destination.first, std::string(e.what()));
    }
    catch(...)
    {
      on_error(destination.first, std::string("unknown exception"));
    }
  }
}

/// Fan a relayed message out to each selected destination, rewriting its
/// per-destination fields for each one (issue #51).
///
/// This composition is the whole point, and it is why the rewrite is not
/// done once above the loop: `applyRelayDestination` overwrites
/// `destination_topic` and the three QoS fields from the recipient's own
/// configuration, so hoisting it would send every remote whichever remote's
/// config happened to be applied last -- the classic fan-out bug, and a
/// silent one, since each destination still receives a well-formed message.
/// Keeping the rewrite inside the per-destination lambda here means one
/// place tests it, rather than the rewrite and the fan-out being correct
/// separately and wired together untested.
///
/// The message is rewritten in place and reused across destinations
/// deliberately: the payload is the expensive part and is never touched.
///
/// @param message the received message, forwarded byte-for-byte apart from
///        the fields applyRelayDestination rewrites
/// @param hub_topic the topic as this bridge knows it (the subscribers_ key)
/// @param destinations the (remote -> connection ids) selection
/// @param config_by_remote per-remote destination topic / QoS, captured
///        under subscribers_mutex_. A remote missing from this map is sent
///        a default-configured message, matching the pre-extraction path.
/// @param send_one called as `send_one(remote_name, connection_ids,
///        message)` after the rewrite for that remote; may throw
/// @param on_error called as `on_error(remote_name, message)` for a
///        destination whose send threw. Must not throw.
template<typename SendFn, typename ErrorFn>
void relayToEachDestination(
  udp_bridge_interfaces::msg::MessageInternal& message,
  const std::string& hub_topic,
  const SelectedConnections& destinations,
  const std::map<std::string, DestinationConfig>& config_by_remote,
  SendFn&& send_one,
  ErrorFn&& on_error)
{
  sendToEachDestination(destinations,
    [&](const std::string& remote_name,
        const std::vector<std::string>& connection_ids)
    {
      auto config_it = config_by_remote.find(remote_name);
      applyRelayDestination(message, hub_topic,
                            config_it != config_by_remote.end()
                              ? config_it->second : DestinationConfig());
      send_one(remote_name, connection_ids, message);
    },
    on_error);
}

} // namespace udp_bridge

#endif

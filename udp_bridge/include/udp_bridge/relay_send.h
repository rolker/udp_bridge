#ifndef UDP_BRIDGE_RELAY_SEND_H
#define UDP_BRIDGE_RELAY_SEND_H

#include <exception>
#include <string>

#include "udp_bridge/connection.h"
#include "udp_bridge/destination_selection.h"

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

} // namespace udp_bridge

#endif

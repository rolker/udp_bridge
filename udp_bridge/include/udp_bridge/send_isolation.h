#ifndef UDP_BRIDGE_SEND_ISOLATION_H
#define UDP_BRIDGE_SEND_ISOLATION_H

#include <exception>
#include <string>

#include "udp_bridge/connection.h"

namespace udp_bridge
{

/// Run one send and report, rather than propagate, anything it throws.
///
/// The three arms are the point, and a single `catch(const std::exception&)`
/// gets all three wrong:
///
///   - **`ConnectionException` has no base class** (`connection.h`), so it
///     is NOT a `std::exception` — and it is what `Connection::send()`
///     throws on its ordinary Timeout path, i.e. the failure the send
///     diagnostics exist to surface. Caught first, and separately.
///   - `std::exception` for everything conventional.
///   - `...` so nothing at all escapes into a caller that is looping over
///     other destinations, or into a plain std::thread with no executor
///     catch-all behind it.
///
/// @param label identifies the failing thing to the error reporter (a
///        remote name, a connection id)
/// @param send_one the call to isolate; may throw
/// @param on_error called as `on_error(label, message)` when it did. Must
///        not throw.
template<typename SendFn, typename ErrorFn>
void callIsolated(const std::string& label, SendFn&& send_one, ErrorFn&& on_error)
{
  try
  {
    send_one();
  }
  catch(const ConnectionException& e)
  {
    // No base class: this must be caught before (and separately from)
    // std::exception, or not at all.
    on_error(label, e.getMessage());
  }
  catch(const std::exception& e)
  {
    on_error(label, std::string(e.what()));
  }
  catch(...)
  {
    on_error(label, std::string("unknown exception"));
  }
}

} // namespace udp_bridge

#endif

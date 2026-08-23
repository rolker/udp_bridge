#ifndef UDP_BRIDGE_RELAY_DROPS_H
#define UDP_BRIDGE_RELAY_DROPS_H

#include <atomic>
#include <cstdint>
#include <string>

namespace udp_bridge
{

/// Why a relay item that was already dequeued never went out (issue #51).
///
/// `RelayQueue::dropped_count()` covers only what the queue itself
/// discards — overflow and push-after-stop. Everything past that point is
/// the sink's (`UDPBridge::relayToOtherRemotes`) decision, and a relay drop
/// is **unrecoverable**: the message never reached a `Connection`, so the
/// resend layer has nothing to retransmit. A sink that returns early
/// without counting makes the `relay queue` diagnostic under-report real
/// loss, which is the one thing that diagnostic exists to show.
enum class RelayDropReason
{
  /// No `source_node`, so the loop rule has nobody to exclude. Refused
  /// deliberately; see destination_selection.h.
  UnnamedSender,
  /// The topic's routing table was torn down between the drain thread's
  /// probe and the sink call.
  TopicGone,
  /// The routing table still lists other remotes, but the per-connection
  /// `period` rule says none is due right now. This is the rate limiter
  /// working as configured, NOT loss — counted separately so it is visible
  /// without inflating the loss figure or the WARN.
  NoDestinationDue,
};

/// Sink-side relay drop tallies. Incremented from the relay worker and read
/// from the diagnostic timer, hence atomic; relaxed ordering is enough
/// because nothing is published through these counters.
struct RelayDropCounters
{
  std::atomic<uint64_t> unnamed_sender {0};
  std::atomic<uint64_t> topic_gone {0};
  std::atomic<uint64_t> no_destination_due {0};

  void record(RelayDropReason reason)
  {
    switch(reason)
    {
      case RelayDropReason::UnnamedSender:
        unnamed_sender.fetch_add(1, std::memory_order_relaxed); break;
      case RelayDropReason::TopicGone:
        topic_gone.fetch_add(1, std::memory_order_relaxed); break;
      case RelayDropReason::NoDestinationDue:
        no_destination_due.fetch_add(1, std::memory_order_relaxed); break;
    }
  }

  /// Items that were meant to be forwarded and were not — the figure that
  /// belongs in the loss total alongside RelayQueue::dropped_count().
  /// Excludes NoDestinationDue, which is the configured rate limit.
  uint64_t lost() const
  {
    return unnamed_sender.load(std::memory_order_relaxed)
         + topic_gone.load(std::memory_order_relaxed);
  }

  /// A short "reason=count" breakdown of the **loss** reasons for the
  /// diagnostic, omitting zeros. Empty when no loss has been recorded.
  /// Deliberately excludes NoDestinationDue: it is reported on its own row
  /// (`rate_limited`) so it never appears alongside a loss figure it is not
  /// part of, and never lands in a WARN string whose trigger excludes it.
  std::string lossBreakdown() const
  {
    std::string out;
    auto append = [&out](const char* name, uint64_t value)
    {
      if(value == 0)
        return;
      if(!out.empty())
        out += ", ";
      out += name;
      out += "=";
      out += std::to_string(value);
    };
    append("unnamed_sender", unnamed_sender.load(std::memory_order_relaxed));
    append("topic_gone", topic_gone.load(std::memory_order_relaxed));
    return out;
  }

  /// Items held back purely by the per-connection `period` rule. Visible,
  /// but not loss — see NoDestinationDue.
  uint64_t rateLimited() const
  {
    return no_destination_due.load(std::memory_order_relaxed);
  }

  // Deliberately no reset(): these counters are monotonic for the process
  // lifetime, like RelayQueue::dropped_count(), and the diagnostic reports
  // deltas against last_reported_relay_drops_. Zeroing them without
  // zeroing that baseline in the same place would underflow
  // `dropped - last_reported_relay_drops_` to ~2^64 and pin the relay
  // diagnostic at WARN.
};

} // namespace udp_bridge

#endif

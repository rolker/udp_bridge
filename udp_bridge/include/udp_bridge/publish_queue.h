#ifndef UDP_BRIDGE_PUBLISH_QUEUE_H
#define UDP_BRIDGE_PUBLISH_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <memory>

#include "rclcpp/serialized_message.hpp"

#include "udp_bridge/relay_item.h"

namespace udp_bridge
{

/// One republish job handed from the socket-drain thread to the publish
/// worker. It carries everything the worker needs to find-or-create the
/// destination publisher and publish, so that NO rmw work (publisher
/// creation, graph queries, publish) happens on the socket-drain thread.
/// See issue #10: a RELIABLE publish() stalling on an uncleanly-dead
/// subscriber — or a first-arrival create_generic_publisher() blocking on
/// rmw discovery — must never stall recvfrom.
struct PublishItem
{
  std::string topic;
  std::string datatype;
  std::string reliability;
  std::string durability;
  uint32_t history_depth = 0;
  rclcpp::SerializedMessage message;

  /// The same message in the form the relay worker needs (issue #51), or
  /// null when this message has nowhere to be relayed — which is every
  /// configuration in this repo today, so the ordinary path allocates
  /// nothing.
  ///
  /// It rides along with the PublishItem so that relay happens *only for
  /// packets the stale/reorder gate admits*, including the buffered ones:
  /// a packet the reorder buffer holds is stored as a PublishItem and
  /// released later (by a gap-filler or by window expiry), and it must
  /// carry its relay form with it — reconstructing a MessageInternal at
  /// release time would mean keeping the payload twice over, and relaying
  /// at buffer time would forward packets the gate may still drop.
  /// UDPBridge::enqueuePublish() is the single place that hands it to
  /// relay_queue_, on every path that publishes.
  std::unique_ptr<RelayItem> relay;

  /// Approximate footprint for the queue's byte budget. The serialized
  /// payload dominates; the small string fields are included so an
  /// empty-payload message still counts a nonzero amount. An attached
  /// relay form is counted too — it is resident memory held by this item
  /// (null, and so free, whenever relay is unreachable).
  size_t byte_size() const
  {
    return message.size()
      + topic.size() + datatype.size()
      + reliability.size() + durability.size()
      + (relay ? relay->byte_size() : 0u);
  }
};

/// Bounded, drop-oldest publish queue served by a single worker thread.
///
/// Decouples the rmw-touching republish tail of UDPBridge::decodeData from
/// the socket-drain thread (issue #10). push() never blocks; the bound is in
/// bytes (a reassembled image is large), and when a new item would exceed the
/// budget the oldest queued item(s) are dropped (KEEP_LAST semantics — newest
/// wins, matching the bridge's documented best-effort-with-loss-reduction
/// model; see doc/qos_design.md) and counted. The actual publish work is an
/// injected sink, so the component carries no dependency on UDPBridge / rclcpp
/// publishers and is unit-testable with a fake sink: a blocking sink proves
/// the non-stall property, a recording sink proves FIFO order.
class PublishQueue
{
public:
  using Sink = std::function<void(PublishItem&&)>;

  PublishQueue() = default;
  ~PublishQueue() { stop(); }

  PublishQueue(const PublishQueue&) = delete;
  PublishQueue& operator=(const PublishQueue&) = delete;

  /// Set the worker's sink and byte budget. Call once before start() (e.g.
  /// in on_configure); changing these while running is not supported.
  void configure(Sink sink, size_t max_bytes)
  {
    sink_ = std::move(sink);
    max_bytes_ = max_bytes;
  }

  /// Launch the worker thread. A no-op if already running.
  void start()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(running_)
      return;
    stop_requested_ = false;
    // Construct the worker BEFORE marking running_: if std::thread throws
    // (std::system_error on resource exhaustion), running_ stays false, so the
    // queue is cleanly not-running — push() drops, start() is retryable —
    // rather than stuck running_=true with no worker to drain. run() never
    // reads running_ (only stop_requested_ / the queue), so this ordering is
    // behaviour-neutral on the success path.
    worker_ = std::thread(&PublishQueue::run, this);
    running_ = true;
  }

  /// Signal the worker to finish and join it. Any items still queued are
  /// discarded (best-effort: prompt, bounded shutdown — the join waits at
  /// most for one in-flight sink call to return, not for the whole backlog
  /// to drain through a possibly-stalled sink). After stop(), push() is a
  /// no-op until the next start(). Safe to call repeatedly and from the
  /// destructor.
  void stop()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(!running_)
        return;
      stop_requested_ = true;
    }
    cv_.notify_all();
    if(worker_.joinable())
      worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    queue_.clear();
    queued_bytes_ = 0;
  }

  /// Enqueue a job. Never blocks. Drops the oldest queued item(s) if adding
  /// this one would exceed the byte budget (a single item larger than the
  /// whole budget is still enqueued so it is not silently lost). A no-op
  /// (counted as a drop) if the worker is not running, so push() is safe to
  /// call from any thread at any lifecycle phase.
  void push(PublishItem&& item)
  {
    const size_t item_bytes = item.byte_size();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(stop_requested_ || !running_)
      {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      while(!queue_.empty() && queued_bytes_ + item_bytes > max_bytes_)
      {
        queued_bytes_ -= queue_.front().byte_size();
        queue_.pop_front();
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
      }
      queued_bytes_ += item_bytes;
      queue_.push_back(std::move(item));
    }
    cv_.notify_one();
  }

  /// Total items dropped due to overflow or push-after-stop.
  uint64_t dropped_count() const
  {
    return dropped_count_.load(std::memory_order_relaxed);
  }

  /// Current queued depth (items). For diagnostics / tests.
  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

private:
  void run()
  {
    for(;;)
    {
      PublishItem item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]{ return stop_requested_ || !queue_.empty(); });
        if(stop_requested_)
          return;  // prompt shutdown; remaining items are discarded in stop()
        item = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= item.byte_size();
      }
      // The sink runs WITHOUT the lock held: a blocking publish here stalls
      // only this worker thread, never push() or the socket-drain thread.
      //
      // The try/catch is a last-resort barrier so a throwing sink can never
      // std::terminate the process. The sink (UDPBridge::publishItem) is
      // expected to catch and log its own exceptions — it has the node
      // logger; this layer does not — but create_generic_publisher (unknown /
      // cross-version datatype), publish(), and sendBridgeInfo()
      // (ConnectionException on socket errors) can all throw, and before
      // issue #10 this work ran inside the socket-drain executor thread whose
      // catch-all logged + dropped bad packets instead of tearing the node
      // down. A plain std::thread has no such backstop, so we add one here.
      if(sink_)
      {
        try
        {
          sink_(std::move(item));
        }
        catch(...)
        {
          // Swallow and continue: the worker must never terminate the
          // process over one bad item. Informative logging belongs to the
          // sink (which has the logger); reaching here means the sink let an
          // exception escape, which it shouldn't.
        }
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<PublishItem> queue_;
  size_t queued_bytes_ = 0;
  size_t max_bytes_ = 0;
  Sink sink_;
  std::thread worker_;
  bool running_ = false;
  bool stop_requested_ = false;
  std::atomic<uint64_t> dropped_count_ {0};
};

} // namespace udp_bridge

#endif

#ifndef UDP_BRIDGE_RELAY_QUEUE_H
#define UDP_BRIDGE_RELAY_QUEUE_H

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

#include "udp_bridge/relay_item.h"

namespace udp_bridge
{

/// Bounded, drop-oldest relay queue served by a single worker thread.
///
/// Same shape and same rationale as PublishQueue (see publish_queue.h):
/// push() never blocks, the bound is in bytes, and when a new item would
/// exceed the budget the oldest queued item(s) are dropped (KEEP_LAST
/// semantics — newest wins, matching the bridge's best-effort model, see
/// doc/qos_design.md) and counted. The forwarding work is an injected sink,
/// so this component carries no dependency on UDPBridge and is unit-testable
/// with a fake sink: a blocking sink proves the non-stall property, a
/// recording sink proves FIFO order.
///
/// Kept as its own queue rather than reusing publish_queue_ so a stalled
/// *local* subscriber and a stalled *remote* link cannot starve each other:
/// they are independent failure domains, and sharing one worker would put
/// Connection::send()'s retry loop in front of local republishes.
class RelayQueue
{
public:
  using Sink = std::function<void(RelayItem&&)>;

  RelayQueue() = default;
  ~RelayQueue() { stop(); }

  RelayQueue(const RelayQueue&) = delete;
  RelayQueue& operator=(const RelayQueue&) = delete;

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
    // Construct the worker BEFORE marking running_, so a throwing
    // std::thread leaves the queue cleanly not-running (push() drops,
    // start() is retryable) rather than running_=true with no worker.
    // Mirrors PublishQueue::start().
    worker_ = std::thread(&RelayQueue::run, this);
    running_ = true;
  }

  /// Signal the worker to finish and join it. Any items still queued are
  /// discarded (best-effort: the join waits at most for one in-flight sink
  /// call to return, not for the whole backlog to drain through a
  /// possibly-stalled sink). After stop(), push() is a no-op until the next
  /// start(). Safe to call repeatedly and from the destructor.
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
  void push(RelayItem&& item)
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
      RelayItem item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]{ return stop_requested_ || !queue_.empty(); });
        if(stop_requested_)
          return;  // prompt shutdown; remaining items are discarded in stop()
        item = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= item.byte_size();
      }
      // The sink runs WITHOUT the lock held: a blocking sendto here stalls
      // only this worker thread, never push() or the socket-drain thread.
      //
      // The try/catch is a last-resort barrier so a throwing sink can never
      // std::terminate the process. The sink (UDPBridge::relayItem) is
      // expected to catch and log its own exceptions — it has the node
      // logger; this layer does not — but Connection::send() can throw
      // ConnectionException on socket errors, and a plain std::thread has no
      // executor catch-all behind it.
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
          // sink, which has the logger.
        }
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<RelayItem> queue_;
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

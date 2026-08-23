// Unit tests for udp_bridge::RelayQueue (issue #51).
//
// The relay queue keeps remote->remote forwarding off the socket-drain
// thread: Connection::send()'s sendto() uses MSG_DONTWAIT with a ~200 ms
// worst-case retry loop when the send buffer is full, so forwarding inline
// in decodeData would stack that cost per stalled destination ahead of the
// next recvfrom — the issue #10 hazard, on the send side. Same contract, and
// the same isolated injected-sink testing, as test_publish_queue.cpp.
//
//   PushDoesNotBlockOnAStalledSink — the property the queue exists for: a
//                                 wedged outgoing link cannot stall the
//                                 caller (the socket-drain thread).
//   DropsOldestOverBudget       — bounded in bytes, newest wins.
//   FifoOrder                   — relays leave in arrival order.
//   PushAfterStopIsCountedNotLost — a push while not running is a counted
//                                 drop, never a crash: push() is called
//                                 from the drain thread at any lifecycle
//                                 phase.
//   OversizeItemEnqueuedThenEvicted — an item larger than the whole budget
//                                 is still enqueued rather than silently
//                                 lost, and is evicted by the next one.
//   StopWhileSinkBlockedJoinsAfterSinkReturns — shutdown is bounded by one
//                                 in-flight sink call (in the field, one
//                                 Connection::send()), not by the backlog.
//   StopCountsTheDiscardedBacklog — and what it throws away is counted, so
//                                 a deactivate cannot silently eat a hub's
//                                 queued fan-out.
//   ConcurrentStopsAreSerialized — two stop() callers must not both reach
//                                 worker_.join().
//   ThrowingSinkDoesNotKillWorker / ConnectionExceptionInSinkDoesNotKillWorker
//                                 — the last-resort barrier. The relay sink
//                                 reaches Connection::send(), which throws
//                                 ConnectionException (NO base class) on
//                                 its ordinary Timeout path, so this is the
//                                 realistic case, not a hypothetical one: a
//                                 plain std::thread has no executor
//                                 catch-all behind it.

#include "udp_bridge/relay_drops.h"
#include "udp_bridge/relay_queue.h"

#include "udp_bridge/connection.h"

#include <atomic>
#include <stdexcept>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using udp_bridge::RelayItem;
using udp_bridge::RelayQueue;

namespace
{

// A RelayItem whose byte_size() is payload_bytes + topic.size().
RelayItem makeItem(const std::string& topic, size_t payload_bytes)
{
  RelayItem item;
  item.topic = topic;
  item.message.data.resize(payload_bytes);
  return item;
}

// Blocks from its first invocation until release(). Declared before the
// queue in each test so the queue (and its worker) is destroyed first.
struct BlockingSink
{
  std::mutex m;
  std::condition_variable cv;
  bool released = false;
  std::atomic<int> calls {0};

  void operator()(RelayItem&&)
  {
    calls.fetch_add(1);
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [this]{ return released; });
  }

  void release()
  {
    {
      std::lock_guard<std::mutex> lock(m);
      released = true;
    }
    cv.notify_all();
  }

  void wait_until_blocked()
  {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while(calls.load() == 0)
    {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline)
        << "relay worker never entered the sink";
      std::this_thread::sleep_for(1ms);
    }
  }
};

} // namespace

TEST(RelayQueue, PushDoesNotBlockOnAStalledSink)
{
  BlockingSink sink;
  {
    RelayQueue queue;
    queue.configure([&sink](RelayItem&& item){ sink(std::move(item)); },
                    1024 * 1024);
    queue.start();

    queue.push(makeItem("/first", 16));
    sink.wait_until_blocked();

    // The worker is parked inside the sink. push() must still return
    // promptly — this is the drain thread in production.
    const auto start = std::chrono::steady_clock::now();
    for(int i = 0; i < 100; ++i)
      queue.push(makeItem("/more", 16));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 500ms) << "push() blocked behind a stalled sink";

    sink.release();
  }
  sink.release();  // idempotent; the queue is already destroyed
}

TEST(RelayQueue, DropsOldestOverBudget)
{
  std::mutex m;
  std::vector<std::string> processed;
  BlockingSink gate;

  RelayQueue queue;
  // Budget fits two 100-byte items but not three.
  queue.configure(
    [&](RelayItem&& item)
    {
      gate(makeItem("", 0));  // hold the worker so nothing drains mid-test
      std::lock_guard<std::mutex> lock(m);
      processed.push_back(item.topic);
    },
    250);
  queue.start();

  queue.push(makeItem("/hold", 100));  // this one parks the worker
  gate.wait_until_blocked();

  queue.push(makeItem("/oldest", 100));
  queue.push(makeItem("/middle", 100));
  EXPECT_EQ(queue.dropped_count(), 0u);
  queue.push(makeItem("/newest", 100));
  EXPECT_EQ(queue.dropped_count(), 1u) << "oldest queued item should be dropped";
  EXPECT_EQ(queue.size(), 2u);

  gate.release();
  queue.stop();
}

TEST(RelayQueue, FifoOrder)
{
  std::mutex m;
  std::condition_variable done;
  std::vector<std::string> processed;

  RelayQueue queue;
  queue.configure(
    [&](RelayItem&& item)
    {
      std::lock_guard<std::mutex> lock(m);
      processed.push_back(item.topic);
      done.notify_all();
    },
    1024 * 1024);
  queue.start();

  queue.push(makeItem("/a", 8));
  queue.push(makeItem("/b", 8));
  queue.push(makeItem("/c", 8));

  {
    std::unique_lock<std::mutex> lock(m);
    ASSERT_TRUE(done.wait_for(lock, 2s, [&]{ return processed.size() == 3; }))
      << "relay worker did not drain the queue";
    EXPECT_EQ(processed, (std::vector<std::string>{"/a", "/b", "/c"}));
  }

  queue.stop();
}

TEST(RelayQueue, PushAfterStopIsCountedNotLost)
{
  RelayQueue queue;
  queue.configure([](RelayItem&&){}, 1024);
  // Never started: push() is a counted no-op, not a crash. decodeData can
  // push at any lifecycle phase.
  queue.push(makeItem("/x", 8));
  EXPECT_EQ(queue.dropped_count(), 1u);

  queue.start();
  queue.stop();
  queue.push(makeItem("/y", 8));
  EXPECT_EQ(queue.dropped_count(), 2u);
}

// An item bigger than the whole budget is enqueued rather than dropped on
// the floor (a reassembled image can exceed a small budget), and the next
// push evicts it. Parity with PublishQueue.OversizeItemEnqueuedThenEvicted.
TEST(RelayQueue, OversizeItemEnqueuedThenEvicted)
{
  BlockingSink sink;
  RelayQueue queue;
  queue.configure([&sink](RelayItem&& item){ sink(std::move(item)); }, 100);
  queue.start();

  queue.push(makeItem("/prime", 64));  // parks the worker
  sink.wait_until_blocked();

  queue.push(makeItem("/big", 500));  // over budget, but the queue is empty
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.dropped_count(), 0u);

  queue.push(makeItem("/big2", 500));  // evicts the first oversize item
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.dropped_count(), 1u);

  sink.release();
}

// stop() joins cleanly while the worker is blocked in the sink: it waits for
// the one in-flight call to return (in the field, one Connection::send()
// with its ~200 ms sendto retry loop), not for the whole backlog. This is
// what bounds on_deactivate.
TEST(RelayQueue, StopWhileSinkBlockedJoinsAfterSinkReturns)
{
  BlockingSink sink;
  RelayQueue queue;
  queue.configure([&sink](RelayItem&& item){ sink(std::move(item)); }, 1024);
  queue.start();

  queue.push(makeItem("/p", 16));
  sink.wait_until_blocked();

  std::atomic<bool> stop_done {false};
  std::thread stopper([&]{ queue.stop(); stop_done.store(true); });

  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(stop_done.load()) << "stop() must wait for the in-flight sink call";

  sink.release();
  stopper.join();
  EXPECT_TRUE(stop_done.load());
}

// The backlog stop() throws away is loss, and unrecoverable loss at that —
// the messages never reached a Connection. It happens on the ordinary
// deactivate path, not only at shutdown, so it has to reach the drop total
// the `relay queue` diagnostic reports; otherwise a deactivate silently
// eats a hub's queued fan-out.
TEST(RelayQueue, StopCountsTheDiscardedBacklog)
{
  BlockingSink sink;
  RelayQueue queue;
  queue.configure([&sink](RelayItem&& item){ sink(std::move(item)); }, 4096);
  queue.start();

  queue.push(makeItem("/held", 16));   // taken by the worker, blocks in the sink
  sink.wait_until_blocked();
  queue.push(makeItem("/queued_a", 16));
  queue.push(makeItem("/queued_b", 16));
  ASSERT_EQ(queue.size(), 2u) << "both must still be queued behind the blocked sink";
  ASSERT_EQ(queue.dropped_count(), 0u) << "nothing has overflowed";

  std::thread stopper([&]{ queue.stop(); });
  // Let stop() latch stop_requested_ before the worker is released, so the
  // worker returns rather than racing to dequeue the backlog first.
  std::this_thread::sleep_for(50ms);
  sink.release();
  stopper.join();

  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.dropped_count(), 2u)
    << "the two items stop() discarded must be counted as drops";
}

// stop() is documented safe to call repeatedly and from the destructor;
// two threads calling it at once must not both reach worker_.join()
// (running_ is cleared only after the join, so the running_ check alone
// does not exclude the second caller).
TEST(RelayQueue, ConcurrentStopsAreSerialized)
{
  RelayQueue queue;
  queue.configure([](RelayItem&&){}, 4096);
  queue.start();

  std::thread a([&]{ queue.stop(); });
  std::thread b([&]{ queue.stop(); });
  a.join();
  b.join();

  // A second round must still work: stop() left the queue cleanly stopped.
  queue.start();
  queue.stop();
  SUCCEED();
}

namespace
{

// Drive `queue` with a throwing item followed by a good one, and return the
// topics the sink processed. Shared by the two barrier tests below.
std::vector<std::string> processedAfterThrow(RelayQueue& queue,
                                             std::mutex& m,
                                             std::vector<std::string>& processed)
{
  queue.start();
  queue.push(makeItem("/boom", 16));   // sink throws — must be swallowed
  queue.push(makeItem("/after", 16));  // the worker must survive and take this

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for(;;)
  {
    {
      std::lock_guard<std::mutex> lock(m);
      if(!processed.empty())
        break;
    }
    EXPECT_LT(std::chrono::steady_clock::now(), deadline)
      << "worker did not survive the throwing sink";
    if(std::chrono::steady_clock::now() >= deadline)
      break;
    std::this_thread::sleep_for(1ms);
  }

  std::lock_guard<std::mutex> lock(m);
  return processed;
}

} // namespace

// Parity with PublishQueue.ThrowingSinkDoesNotKillWorker: a std::exception
// out of the sink must not std::terminate the process.
TEST(RelayQueue, ThrowingSinkDoesNotKillWorker)
{
  std::mutex m;
  std::vector<std::string> processed;

  RelayQueue queue;
  queue.configure(
    [&](RelayItem&& item)
    {
      if(item.topic == "/boom")
        throw std::runtime_error("sink failure");
      std::lock_guard<std::mutex> lock(m);
      processed.push_back(item.topic);
    },
    1024 * 1024);

  EXPECT_EQ(processedAfterThrow(queue, m, processed),
            (std::vector<std::string>{"/after"}));
  queue.stop();
}

// The realistic case for THIS queue: Connection::send() throws
// ConnectionException, which has no base class, so it is only ever caught by
// a catch(...). relayToOtherRemotes catches it per destination and logs; if
// a future change lets one escape, the queue's last-resort barrier must
// still keep the worker alive.
TEST(RelayQueue, ConnectionExceptionInSinkDoesNotKillWorker)
{
  std::mutex m;
  std::vector<std::string> processed;

  RelayQueue queue;
  queue.configure(
    [&](RelayItem&& item)
    {
      if(item.topic == "/boom")
        throw udp_bridge::ConnectionException("Timeout");
      std::lock_guard<std::mutex> lock(m);
      processed.push_back(item.topic);
    },
    1024 * 1024);

  EXPECT_EQ(processedAfterThrow(queue, m, processed),
            (std::vector<std::string>{"/after"}));
  queue.stop();
}

// Sink-side drops (issue #51). RelayQueue::dropped_count() sees only what
// the queue discards; an item the sink dequeues and then abandons — the
// sender is unnamed, or the topic's routing table was torn down under it —
// is loss the queue cannot count. A relay drop is
// unrecoverable (the message never reached a Connection, so the resend
// layer has nothing to retransmit), so the diagnostic must sum both or it
// under-reports the thing it exists to show.
TEST(RelayDropCountersTest, SinkDropsAreCountedAsLoss)
{
  udp_bridge::RelayDropCounters counters;
  EXPECT_EQ(counters.lost(), 0u);
  EXPECT_TRUE(counters.breakdown().empty());

  counters.record(udp_bridge::RelayDropReason::UnnamedSender);
  counters.record(udp_bridge::RelayDropReason::TopicGone);
  counters.record(udp_bridge::RelayDropReason::TopicGone);

  EXPECT_EQ(counters.lost(), 3u) << "every abandoned relay item is loss";
  EXPECT_EQ(counters.unnamed_sender.load(), 1u);
  EXPECT_EQ(counters.topic_gone.load(), 2u);

  const auto breakdown = counters.breakdown();
  EXPECT_NE(breakdown.find("unnamed_sender=1"), std::string::npos);
  EXPECT_NE(breakdown.find("topic_gone=2"), std::string::npos);
  EXPECT_EQ(breakdown.find("rate_limited"), std::string::npos)
    << "zero-valued reasons stay out of the diagnostic string";
}

// The fourth early return is NOT loss: the routing table lists other
// remotes but none is due under its `period` yet. Counting it as a drop
// would put the relay diagnostic permanently in WARN on any rate-limited
// topic, which is exactly how a real-loss signal gets ignored.
TEST(RelayDropCountersTest, RateLimitedIsVisibleButNotLoss)
{
  udp_bridge::RelayDropCounters counters;
  counters.record(udp_bridge::RelayDropReason::NoDestinationDue);
  counters.record(udp_bridge::RelayDropReason::NoDestinationDue);

  EXPECT_EQ(counters.lost(), 0u) << "the rate limiter working is not relay loss";
  EXPECT_EQ(counters.no_destination_due.load(), 2u);
  EXPECT_NE(counters.breakdown().find("rate_limited=2"), std::string::npos)
    << "it must still be visible to an operator reading the diagnostic";

  counters.reset();
  EXPECT_EQ(counters.lost(), 0u);
  EXPECT_TRUE(counters.breakdown().empty());
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// Unit tests for udp_bridge::PublishQueue (issue #10).
//
// The queue decouples the rmw-touching republish tail of decodeData from the
// socket-drain thread so a stalled publish() can't wedge recvfrom. These
// tests exercise that contract in isolation via an injected sink — no node,
// no DDS. The blocking-sink test is the in-process proxy for the field bug
// (a RELIABLE publish stalling on an uncleanly-dead subscriber); the live-DDS
// kill-subscriber reproduction lives in the issue #18 bench harness.

#include "udp_bridge/publish_queue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using udp_bridge::PublishItem;
using udp_bridge::PublishQueue;

namespace
{

// Build a PublishItem whose byte_size() is (payload_bytes + topic.size()).
// We never read the payload — only its accounted size matters here.
PublishItem makeItem(const std::string& topic, size_t payload_bytes)
{
  PublishItem item;
  item.topic = topic;
  if(payload_bytes > 0)
  {
    item.message.reserve(payload_bytes);
    item.message.get_rcl_serialized_message().buffer_length = payload_bytes;
  }
  return item;
}

// A sink that blocks on a latch from its first invocation until release().
// Declared so a test can construct it BEFORE the PublishQueue, guaranteeing
// the queue (and its worker) is destroyed first — i.e. the latch outlives the
// worker that waits on it. Always release() before the test ends.
struct BlockingSink
{
  std::mutex m;
  std::condition_variable cv;
  bool released = false;
  std::atomic<int> calls {0};

  void operator()(PublishItem&&)
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

  // Wait (bounded) until the worker is parked inside the sink (has popped an
  // item). Fails fast with a clear message instead of spinning forever if the
  // worker never runs (a regression, start() failure, or scheduling stall) —
  // an unbounded spin would otherwise surface only as an opaque colcon test
  // timeout.
  void wait_until_blocked()
  {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while(calls.load() == 0)
    {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline)
        << "publish worker never entered the sink";
      std::this_thread::sleep_for(1ms);
    }
  }
};

} // namespace

// The core invariant and the bug: push() must never block on a stalled sink.
// With the worker parked inside a never-returning sink, a burst of pushes must
// still return effectively immediately.
TEST(PublishQueue, PushDoesNotBlockOnStalledSink)
{
  BlockingSink sink;
  PublishQueue q;
  q.configure([&sink](PublishItem&& i){ sink(std::move(i)); },
              /*max_bytes=*/ 1024u * 1024u);
  q.start();

  // Occupy the worker: it pops this item and blocks in the sink.
  q.push(makeItem("prime", 64));
  sink.wait_until_blocked();

  // Now the consumer is wedged. Pushing must not be.
  const auto t0 = std::chrono::steady_clock::now();
  for(int i = 0; i < 200; ++i)
    q.push(makeItem("t", 64));
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  // 200 pushes against a wedged consumer in well under a second (real cost is
  // microseconds; the bound is generous to avoid CI flakiness).
  EXPECT_LT(elapsed, 1s);

  sink.release();  // let the worker drain + the dtor join cleanly
}

// FIFO order through the worker.
TEST(PublishQueue, PreservesFifoOrder)
{
  std::mutex order_mutex;
  std::vector<std::string> order;

  // Gate the worker so all items are queued before any is processed; once
  // opened, every sink call returns immediately, so processing is in queue
  // (FIFO) order.
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool open = false;

  PublishQueue q;
  q.configure(
    [&](PublishItem&& item)
    {
      {
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_cv.wait(lock, [&]{ return open; });
      }
      std::lock_guard<std::mutex> lock(order_mutex);
      order.push_back(item.topic);
    },
    /*max_bytes=*/ 1024u * 1024u);
  q.start();

  const std::vector<std::string> topics = {"a", "b", "c", "d", "e"};
  for(const auto& t : topics)
    q.push(makeItem(t, 16));

  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    open = true;
  }
  gate_cv.notify_all();

  // Wait (bounded) for all items to be processed before stopping — stop()
  // discards anything still queued.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for(;;)
  {
    {
      std::lock_guard<std::mutex> lock(order_mutex);
      if(order.size() == topics.size())
        break;
    }
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "worker did not drain in time";
    std::this_thread::sleep_for(1ms);
  }
  q.stop();

  EXPECT_EQ(order, topics);
}

// Over the byte budget, push() drops the oldest queued item(s) and counts them.
TEST(PublishQueue, DropsOldestOverByteBudget)
{
  BlockingSink sink;
  // Budget holds three 101-byte items (303) but not four (404).
  PublishQueue q;
  q.configure([&sink](PublishItem&& i){ sink(std::move(i)); }, /*max_bytes=*/ 350u);
  q.start();

  // Park the worker so nothing drains; the queue then exercises pure push
  // overflow logic. The prime item (byte_size 101) is popped, not queued.
  q.push(makeItem("p", 100));  // topic "p" (1) + 100 = 101 bytes
  sink.wait_until_blocked();

  // Each item is 101 bytes ("i" + 100). Push five.
  for(int i = 0; i < 5; ++i)
    q.push(makeItem("i", 100));

  // Budget 350 holds 3 (303); the 4th and 5th each evict the oldest.
  EXPECT_EQ(q.size(), 3u);
  EXPECT_EQ(q.dropped_count(), 2u);

  sink.release();
}

// A single item larger than the whole budget is still enqueued (never silently
// lost), and the next push evicts it to make room.
TEST(PublishQueue, OversizeItemEnqueuedThenEvicted)
{
  BlockingSink sink;
  PublishQueue q;
  q.configure([&sink](PublishItem&& i){ sink(std::move(i)); }, /*max_bytes=*/ 100u);
  q.start();

  q.push(makeItem("p", 64));  // prime → worker blocks
  sink.wait_until_blocked();

  q.push(makeItem("big", 500));  // 503 bytes > budget, but queue empty → enqueued
  EXPECT_EQ(q.size(), 1u);
  EXPECT_EQ(q.dropped_count(), 0u);

  q.push(makeItem("big2", 500));  // evicts the first oversize item
  EXPECT_EQ(q.size(), 1u);
  EXPECT_EQ(q.dropped_count(), 1u);

  sink.release();
}

// push() after stop() is a no-op (counted as a drop), so it is safe to call at
// any lifecycle phase — including the deactivate teardown window.
TEST(PublishQueue, PushAfterStopIsDropped)
{
  PublishQueue q;
  q.configure([](PublishItem&&){}, /*max_bytes=*/ 1024u);
  q.start();
  q.stop();

  q.push(makeItem("x", 16));
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.dropped_count(), 1u);
}

// push() before the worker is ever started is also a safe no-op drop.
TEST(PublishQueue, PushBeforeStartIsDropped)
{
  PublishQueue q;
  q.configure([](PublishItem&&){}, /*max_bytes=*/ 1024u);
  q.push(makeItem("x", 16));
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.dropped_count(), 1u);
}

// stop() joins cleanly even while the worker is blocked in the sink — it waits
// for the one in-flight sink call to return, then returns. This bounds
// shutdown to a single sink call (in the field, the rmw publish's
// max_blocking_time) rather than the whole backlog.
TEST(PublishQueue, StopWhileSinkBlockedJoinsAfterSinkReturns)
{
  BlockingSink sink;
  PublishQueue q;
  q.configure([&sink](PublishItem&& i){ sink(std::move(i)); }, /*max_bytes=*/ 1024u);
  q.start();

  q.push(makeItem("p", 16));
  sink.wait_until_blocked();

  std::atomic<bool> stop_done {false};
  std::thread stopper([&]{ q.stop(); stop_done.store(true); });

  // While the sink is blocked, stop() cannot complete.
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(stop_done.load());

  // Releasing the sink lets the in-flight call return; stop() then joins.
  sink.release();
  stopper.join();
  EXPECT_TRUE(stop_done.load());
}

// A throwing sink must not terminate the worker: PublishQueue::run wraps the
// sink call in a last-resort barrier (issue #10 — the sink, UDPBridge::
// publishItem, can throw from create_generic_publisher / publish /
// sendBridgeInfo, and a plain std::thread would otherwise std::terminate).
// The worker must keep draining subsequent items.
TEST(PublishQueue, ThrowingSinkDoesNotKillWorker)
{
  std::mutex m;
  std::vector<std::string> processed;

  PublishQueue q;
  q.configure(
    [&](PublishItem&& item)
    {
      if(item.topic == "boom")
        throw std::runtime_error("sink failure");
      std::lock_guard<std::mutex> lock(m);
      processed.push_back(item.topic);
    },
    /*max_bytes=*/ 1024u * 1024u);
  q.start();

  q.push(makeItem("boom", 16));   // sink throws — must be swallowed
  q.push(makeItem("after", 16));  // worker must survive and process this

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for(;;)
  {
    {
      std::lock_guard<std::mutex> lock(m);
      if(!processed.empty())
        break;
    }
    ASSERT_LT(std::chrono::steady_clock::now(), deadline)
      << "worker did not survive the throwing sink";
    std::this_thread::sleep_for(1ms);
  }

  std::lock_guard<std::mutex> lock(m);
  ASSERT_EQ(processed.size(), 1u);
  EXPECT_EQ(processed.front(), "after");
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

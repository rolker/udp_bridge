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

#include "udp_bridge/relay_queue.h"

#include <atomic>
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

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

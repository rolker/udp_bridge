// Concurrency test for Connection::send rate-limit accounting.
//
// PR #16 (commit 1489cfb) moved the per-packet can_send / sendto /
// result-add into a single sent_packet_statistics_mutex_ critical
// section, closing a check-then-record TOCTOU that PR #12's audit
// had left split. Without that fix, multiple forwarding callbacks
// running concurrently (the republish_group_ became Reentrant in
// PR #12) could all observe capacity in their separate check critical
// sections and collectively exceed maximum_bytes_per_second.
//
// This test exercises that contention by spinning many threads calling
// Connection::send on a single rate-limited Connection with a fixed
// timestamp (so the per-second window is well-defined). With the fix
// in place, the post-run accounting shows success_bytes_per_second
// strictly below the configured rate limit. Without the fix, a
// regression to separated check/record critical sections would let
// the success total exceed the limit under load.
//
// Note: this is a stress test for a race window. It cannot prove the
// absence of the race — it can only catch a regression once the
// regression manifests under contention. The window without the fix
// is small but reliably hit at the parameters below on a multi-core
// machine.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/statistics.h"

namespace
{

// Bind a UDP socket on 127.0.0.1 to an ephemeral port. Returns the fd
// (caller closes) and writes the port into *port.
int bind_localhost_listener(uint16_t* port)
{
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if(sock < 0)
    return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if(bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
  {
    close(sock);
    return -1;
  }
  socklen_t addr_len = sizeof(addr);
  if(getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0)
  {
    close(sock);
    return -1;
  }
  *port = ntohs(addr.sin_port);
  return sock;
}

} // namespace

class ConnectionRateLimit : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
  }
};

TEST_F(ConnectionRateLimit, ConcurrentSendsStayUnderLimit)
{
  uint16_t listener_port = 0;
  int listener_sock = bind_localhost_listener(&listener_port);
  ASSERT_GE(listener_sock, 0) << "Failed to bind listener socket: " << strerror(errno);

  // Source socket the Connection will sendto() from.
  int send_sock = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(send_sock, 0) << "Failed to open send socket: " << strerror(errno);

  udp_bridge::Connection conn("rate-limit-test", "127.0.0.1", listener_port, "", 0);
  constexpr uint32_t kRateLimitBytesPerSec = 10000;
  conn.setRateLimit(kRateLimitBytesPerSec);

  // Use a fixed timestamp so every send falls in the same 1-second
  // can_send window. can_send sums non-dropped sizes < 1 second old;
  // with all sends at t0, the window is well-defined and the test is
  // deterministic in expected sequential outcome (~9 successes at
  // 1000 bytes each before the 10th hits the cap).
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  constexpr int kNumThreads = 8;
  constexpr int kPacketSize = 1000;
  constexpr int kPacketsPerThread = 200;  // 8 * 200 = 1600 attempts; ~9 should succeed.

  std::atomic<bool> go{false};

  auto worker = [&] {
    std::vector<uint8_t> data(kPacketSize, 0xAB);
    // Bunch up the start so threads contend on the mutex.
    while(!go.load(std::memory_order_acquire))
      std::this_thread::yield();
    for(int i = 0; i < kPacketsPerThread; ++i)
    {
      conn.send(data, send_sock, udp_bridge::PacketSendCategory::message, t0);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for(int i = 0; i < kNumThreads; ++i)
    threads.emplace_back(worker);
  go.store(true, std::memory_order_release);
  for(auto& t : threads)
    t.join();

  auto rates = conn.data_sent_rate(t0, udp_bridge::PacketSendCategory::message);

  // With the fix, success_bytes_per_second must be strictly below the
  // rate limit (can_send uses strict less-than). Without the fix, the
  // TOCTOU lets concurrent callbacks all pass the check and add their
  // bytes, so the success total can climb above the cap.
  EXPECT_LT(rates.success_bytes_per_second, kRateLimitBytesPerSec)
    << "Concurrent Connection::send invocations exceeded the per-second rate "
       "limit (" << rates.success_bytes_per_second << " bytes vs cap "
    << kRateLimitBytesPerSec << "). Suspected regression in the "
       "sent_packet_statistics_mutex_ contract — check that the can_send "
       "check, the sendto, and the result-add all happen under one lock "
       "acquisition in Connection::send.";

  // Sanity: the drop counter accounts for the overflow. With kNumThreads
  // * kPacketsPerThread = 1600 attempts and ~9 expected successes, the
  // remainder are dropped (or failed if sendto errored, but loopback
  // sendto should succeed). This confirms the test is actually
  // exercising the rate-limit path rather than a no-op.
  EXPECT_GT(rates.dropped_bytes_per_second, 0.0f)
    << "Test did not exercise the rate-limit path — no dropped packets "
       "recorded. Tune kRateLimitBytesPerSec or kPacketsPerThread.";

  close(listener_sock);
  close(send_sock);
}

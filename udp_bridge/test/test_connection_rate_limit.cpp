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
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/statistics.h"

namespace
{

// RAII wrapper around a socket fd. Closes the descriptor on destruction
// so a failed ASSERT in the middle of test setup cannot leak it for the
// rest of the test process.
class SocketFd
{
public:
  SocketFd() = default;
  explicit SocketFd(int fd) : fd_(fd) {}
  ~SocketFd() { reset(); }

  SocketFd(const SocketFd&) = delete;
  SocketFd& operator=(const SocketFd&) = delete;

  SocketFd(SocketFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  SocketFd& operator=(SocketFd&& other) noexcept
  {
    if(this != &other)
    {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void reset()
  {
    if(fd_ >= 0)
    {
      close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_ = -1;
};

// Bind a UDP socket on 127.0.0.1 to an ephemeral port. Returns the
// SocketFd (which owns/closes the descriptor) and writes the port into
// *port. On failure the returned SocketFd is invalid (fd_ == -1).
SocketFd bind_localhost_listener(uint16_t* port)
{
  SocketFd sock(socket(AF_INET, SOCK_DGRAM, 0));
  if(!sock.valid())
    return sock;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if(bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    return SocketFd{};  // RAII closes the partial socket
  socklen_t addr_len = sizeof(addr);
  if(getsockname(sock.get(), reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0)
    return SocketFd{};
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
  SocketFd listener_sock = bind_localhost_listener(&listener_port);
  ASSERT_TRUE(listener_sock.valid()) << "Failed to bind listener socket: " << strerror(errno);

  // Source socket the Connection will sendto() from.
  SocketFd send_sock(socket(AF_INET, SOCK_DGRAM, 0));
  ASSERT_TRUE(send_sock.valid()) << "Failed to open send socket: " << strerror(errno);

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
      conn.send(data, send_sock.get(), udp_bridge::PacketSendCategory::message, t0);
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

  // listener_sock and send_sock are SocketFd RAII wrappers — they close
  // on scope exit, so no explicit close() is needed here, and a failed
  // ASSERT earlier in the test cannot leak the descriptors.
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

// Regression tests for adaptive per-connection admission control
// (issue #43).
//
// AIMD on Connection::effective_rate_limit_, driven by the
// delivered-vs-sent ratio from the remote's echoed BridgeInfo
// received_bytes_per_second (the forward-path signal the 2026-08-04
// analysis showed matters: boat->op wifi at 95% loss while the return
// path stayed healthy), with a stale-feedback fallback derived from
// last_receive_time. Pinned contract points:
//
//   DecreaseOnCongestion        — delivery < 90% of sent halves the cap.
//   AdditiveRecovery            — clean delivery recovers 10% of the
//                                 configured limit per feedback sample.
//   FloorClamp                  — repeated congestion never drops below
//                                 admission_floor_fraction * limit.
//   CeilingClamp                — recovery never exceeds the configured
//                                 limit.
//   FeedbackStaleBackoff        — stale last_receive_time is congestion
//                                 even when the reported delivery is high.
//   NeverReceivedSentinel       — last_receive_time 0.0 ("nothing yet")
//                                 is NOT stale; an idle link recovers.
//   EffectiveLimitAffectsCanSend— send() actually meters against the
//                                 reduced cap.
//   SetRateLimitClampPreservesBackoff — re-applying an unchanged rate
//                                 limit (CONNECT flap) must not wipe
//                                 accumulated backoff (review-plan S2).
//
// Sends go to a loopback listener with fixed timestamps so the strict
// 1-second accounting windows are well-defined, as in
// test_connection_rate_limit.cpp / test_resend_budget.cpp.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/resend_constants.h"
#include "udp_bridge/statistics.h"

namespace
{

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
    return SocketFd{};
  socklen_t addr_len = sizeof(addr);
  if(getsockname(sock.get(), reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0)
    return SocketFd{};
  *port = ntohs(addr.sin_port);
  return sock;
}

constexpr uint32_t kRateLimit = 100000;
constexpr int kPacketSize = 1000;

}  // namespace

class AdmissionControl : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
    listener_sock_ = bind_localhost_listener(&listener_port_);
    ASSERT_TRUE(listener_sock_.valid())
      << "Failed to bind listener socket: " << strerror(errno);
    send_sock_ = SocketFd(socket(AF_INET, SOCK_DGRAM, 0));
    ASSERT_TRUE(send_sock_.valid())
      << "Failed to open send socket: " << strerror(errno);
  }

  std::unique_ptr<udp_bridge::Connection> make_connection()
  {
    auto conn = std::make_unique<udp_bridge::Connection>(
      "admission-test", "127.0.0.1", listener_port_, "", 0);
    conn->setRateLimit(kRateLimit);
    return conn;
  }

  // Record ~10 kB of sent traffic at t so the delivered-vs-sent
  // comparison has a sent rate to work against.
  void send_traffic(udp_bridge::Connection& conn, rclcpp::Time t, int packets = 10)
  {
    std::vector<uint8_t> data(kPacketSize, 0xCD);
    for(int i = 0; i < packets; ++i)
      conn.send(data, send_sock_.get(), udp_bridge::PacketSendCategory::message, t);
  }

  SocketFd listener_sock_;
  SocketFd send_sock_;
  uint16_t listener_port_ = 0;
};

TEST_F(AdmissionControl, DecreaseOnCongestion)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0);                       // sent ~10 kB
  conn->update_last_receive_time(t0.seconds(), 100, false);  // feedback fresh

  // Remote reports receiving half of what we sent — congested.
  conn->updateAdmissionControl(5000.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "Delivery at 50% of sent must apply one multiplicative decrease.";
}

TEST_F(AdmissionControl, AdditiveRecovery)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  conn->updateAdmissionControl(5000.0f, t0);     // halve: 50000
  conn->updateAdmissionControl(10000.0f, t0);    // clean (100% delivered)

  const uint32_t expected = static_cast<uint32_t>(
    kRateLimit * udp_bridge::kAdmissionDecreaseFactor +
    udp_bridge::kAdmissionAdditiveStepFraction * kRateLimit);
  EXPECT_EQ(conn->effectiveRateLimit(), expected)
    << "One clean feedback sample must recover additively by "
       "kAdmissionAdditiveStepFraction of the configured limit.";
}

TEST_F(AdmissionControl, FloorClamp)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  for(int i = 0; i < 10; ++i)
    conn->updateAdmissionControl(0.0f, t0);      // total loss, repeatedly

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(udp_bridge::kDefaultAdmissionFloorFraction * kRateLimit))
    << "Repeated congestion must clamp at the admission floor, not zero.";
}

TEST_F(AdmissionControl, CeilingClamp)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  for(int i = 0; i < 5; ++i)
    conn->updateAdmissionControl(10000.0f, t0);  // clean, repeatedly

  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit)
    << "Recovery must never exceed the configured maximum_bytes_per_second.";
}

TEST_F(AdmissionControl, FeedbackStaleBackoff)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0);
  // Nothing received for 2x the starvation threshold: feedback stale.
  conn->update_last_receive_time(
    t0.seconds() - 2.0 * udp_bridge::kAckStarvationThreshold.count(), 100, false);

  // Even a high reported delivery must not mask a dead inbound path —
  // that report is by definition old news.
  conn->updateAdmissionControl(10000.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "Stale feedback (nothing received for kAckStarvationThreshold) "
       "must count as congestion regardless of the reported delivery.";
}

TEST_F(AdmissionControl, NeverReceivedSentinel)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  // No traffic sent, nothing ever received (0.0 sentinel): an idle,
  // brand-new connection is NOT congested — the cap stays at the
  // configured limit (additive step is ceiling-clamped).
  conn->updateAdmissionControl(0.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit)
    << "last_receive_time 0.0 (never received) must not read as stale "
       "feedback — new connections start at the full cap.";
}

TEST_F(AdmissionControl, EffectiveLimitAffectsCanSend)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  // Trigger the decrease via the stale-feedback path so the sent-stats
  // deque holds ONLY the metered sends below — with every record at
  // one timestamp the rate fields are raw byte sums and the assertions
  // are exact, as in test_connection_rate_limit.cpp.
  conn->update_last_receive_time(
    t0.seconds() - 2.0 * udp_bridge::kAckStarvationThreshold.count(), 100, false);
  conn->updateAdmissionControl(0.0f, t0);        // stale => halve to 50000
  ASSERT_EQ(conn->effectiveRateLimit(), kRateLimit / 2);

  // Offer 2x the configured limit in one instant: shedding must occur
  // at the REDUCED cap, not the configured one.
  std::vector<uint8_t> data(kPacketSize, 0xEF);
  for(int i = 0; i < 200; ++i)                   // 200 kB offered
    conn->send(data, send_sock_.get(), udp_bridge::PacketSendCategory::message, t0);

  auto rates = conn->data_sent_rate(t0, udp_bridge::PacketSendCategory::message);
  EXPECT_LT(rates.success_bytes_per_second, static_cast<float>(kRateLimit / 2))
    << "send() must meter against the AIMD-reduced effective cap "
       "(strictly below it, per can_send's strict less-than).";
  EXPECT_GE(rates.success_bytes_per_second, 0.5f * (kRateLimit / 2))
    << "Far below the reduced cap — over-throttling regression.";
  EXPECT_GT(rates.dropped_bytes_per_second, 0.0f)
    << "Offered load above the reduced cap must produce drops.";
}

TEST_F(AdmissionControl, SetRateLimitClampPreservesBackoff)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  conn->updateAdmissionControl(5000.0f, t0);     // halve to 50000
  ASSERT_EQ(conn->effectiveRateLimit(), kRateLimit / 2);

  // A CONNECT/addRemote flap re-applies the same configured limit.
  conn->setRateLimit(kRateLimit);
  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit / 2)
    << "Re-applying an unchanged rate limit must not wipe accumulated "
       "AIMD backoff (review-plan S2: clamp, never reset).";

  // A genuinely LOWERED limit takes effect immediately via the clamp.
  conn->setRateLimit(kRateLimit / 4);
  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit / 4)
    << "A lowered configured limit must clamp the effective cap down.";
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

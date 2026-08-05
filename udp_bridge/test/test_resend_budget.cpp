// Regression tests for the sender-side resend budget (issue #44).
//
// Resend storms self-amplify: under ack starvation the sender treats
// all in-flight packets as lost and retries them at multiples of link
// capacity, sustaining the storm until restart (observed 2026-05-01:
// 13 MB/s of retries vs a 1.0 MB/s VPN link; 2026-08-04: ~1.5 MB/s
// dropped + 220 kB/s sent resends vs a 1.2 MB/s cap).
//
// Connection::resend_packets bounds the resend category to
// resend_budget_fraction (default kDefaultResendBudgetFraction) of the
// connection's rate limit, measured over the same strict 1-second
// window can_send uses, and halves that budget per
// kAckStarvationThreshold elapsed since the last received packet
// (clamped at kMaxAckStarvationBackoffShift halvings). These tests pin:
//
//   1. BoundedUnderLoad     — resend bytes never exceed the budget even
//                             when far more is requested (the #18 bench
//                             harness scenario "resend rate stays
//                             bounded", inline until that harness
//                             exists).
//   2. OneBackoffStep       — last_receive_time 1.5 x threshold ago =>
//                             exactly one halving (floor(1.5) = 1).
//   3. NeverReceivedSentinel— last_receive_time_ == 0.0 means "nothing
//                             received yet", NOT "starved forever": a
//                             fresh connection gets the FULL budget.
//   4. MaxBackoffClamp      — starvation far beyond the clamp floors
//                             the budget at 1/2^kMaxAckStarvationBackoffShift,
//                             not lower (the deliberate probe trickle).
//
// Packets are seeded via record_sent_packet_for_test with a non-zero
// payload size (empty payloads would make every budget comparison
// trivially pass). Sends go to a loopback listener; a fixed timestamp
// keeps the 1-second accounting window well-defined, as in
// test_connection_rate_limit.cpp.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/resend_constants.h"
#include "udp_bridge/statistics.h"

namespace
{

// RAII socket wrapper, as in test_connection_rate_limit.cpp.
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

constexpr uint32_t kRateLimitBytesPerSec = 100000;
constexpr int kPacketSize = 1000;
constexpr int kNumPackets = 100;  // 100 kB requested — 4x the default budget.

// Full budget at the default fraction: 0.25 * 100000 = 25000 bytes.
const double kFullBudget =
  static_cast<double>(udp_bridge::kDefaultResendBudgetFraction) * kRateLimitBytesPerSec;

}  // namespace

class ResendBudget : public ::testing::Test
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

  // Build a rate-limited connection seeded with kNumPackets sendable
  // packets, returning the missing-packet list that requests them all.
  std::unique_ptr<udp_bridge::Connection> make_seeded_connection(
    rclcpp::Time t0, std::vector<uint64_t>* missing)
  {
    auto conn = std::make_unique<udp_bridge::Connection>(
      "resend-budget-test", "127.0.0.1", listener_port_, "", 0);
    conn->setRateLimit(kRateLimitBytesPerSec);
    missing->resize(kNumPackets);
    std::iota(missing->begin(), missing->end(), 1);
    for(auto n: *missing)
      conn->record_sent_packet_for_test(n, t0, kPacketSize);
    return conn;
  }

  SocketFd listener_sock_;
  SocketFd send_sock_;
  uint16_t listener_port_ = 0;
};

TEST_F(ResendBudget, BoundedUnderLoad)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  std::vector<uint64_t> missing;
  auto conn = make_seeded_connection(t0, &missing);
  // Recent inbound activity: no starvation backoff.
  conn->update_last_receive_time(t0.seconds(), 100, false);

  conn->resend_packets(missing, send_sock_.get(), t0);

  auto rates = conn->data_sent_rate(t0, udp_bridge::PacketSendCategory::resend);

  // The budget admits floor(25000 / 1000) = 25 packets; the 26th would
  // exceed it. Strict bound first (the regression this test exists
  // for), then a lower bound proving the budget is actually used.
  EXPECT_LE(rates.success_bytes_per_second, kFullBudget)
    << "Resend bytes exceeded the resend budget "
       "(fraction * rate limit). The issue #44 bound is not being "
       "enforced in Connection::resend_packets.";
  EXPECT_GE(rates.success_bytes_per_second, 0.9 * kFullBudget)
    << "Resend bytes far below the budget — the budget is "
       "over-throttling (or sends are failing on loopback).";

  // The remainder must be recorded as dropped resends so shedding is
  // visible in the per-connection DataRates (the stats our field
  // analysis reads).
  EXPECT_GT(rates.dropped_bytes_per_second, 0.0f)
    << "Over-budget resends were not recorded as dropped — shedding "
       "would be invisible in BridgeInfo resend stats.";
}

TEST_F(ResendBudget, OneBackoffStep)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  std::vector<uint64_t> missing;
  auto conn = make_seeded_connection(t0, &missing);
  // 1.5 x threshold of starvation => floor(1.5) = exactly one halving.
  // (2.0 x would floor to TWO steps — a 4x reduction — which is why
  // this test uses 1.5; see the plan-review note on the off-by-one.)
  conn->update_last_receive_time(
    t0.seconds() - 1.5 * udp_bridge::kAckStarvationThreshold.count(), 100, false);

  conn->resend_packets(missing, send_sock_.get(), t0);

  auto rates = conn->data_sent_rate(t0, udp_bridge::PacketSendCategory::resend);

  const double half_budget = kFullBudget / 2.0;
  EXPECT_LE(rates.success_bytes_per_second, half_budget)
    << "One ack-starvation step did not halve the resend budget.";
  EXPECT_GE(rates.success_bytes_per_second, 0.9 * half_budget)
    << "Budget reduced by more than one halving at 1.5x "
       "kAckStarvationThreshold — backoff step accounting is off "
       "(expected exactly floor(1.5) = 1 step).";
}

TEST_F(ResendBudget, NeverReceivedSentinel)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  std::vector<uint64_t> missing;
  auto conn = make_seeded_connection(t0, &missing);
  // Deliberately NO update_last_receive_time call: last_receive_time()
  // returns the 0.0 "nothing received yet" sentinel.

  conn->resend_packets(missing, send_sock_.get(), t0);

  auto rates = conn->data_sent_rate(t0, udp_bridge::PacketSendCategory::resend);

  // The sentinel must read as zero starvation (full budget). Computing
  // starvation from (now - 0.0) would instead produce a huge interval
  // and clamp to max backoff — the plan-review must-fix this test pins.
  EXPECT_GE(rates.success_bytes_per_second, 0.9 * kFullBudget)
    << "A connection that has never received a packet was treated as "
       "ack-starved (last_receive_time 0.0 sentinel mishandled) — new "
       "connections must start with the full resend budget.";
  EXPECT_LE(rates.success_bytes_per_second, kFullBudget);
}

TEST_F(ResendBudget, MaxBackoffClamp)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  std::vector<uint64_t> missing;
  auto conn = make_seeded_connection(t0, &missing);
  // Starved for 100x the threshold — far beyond the clamp. The shift
  // must pin at kMaxAckStarvationBackoffShift, keeping a probe trickle
  // flowing rather than shutting resends off entirely.
  conn->update_last_receive_time(
    t0.seconds() - 100.0 * udp_bridge::kAckStarvationThreshold.count(), 100, false);

  conn->resend_packets(missing, send_sock_.get(), t0);

  auto rates = conn->data_sent_rate(t0, udp_bridge::PacketSendCategory::resend);

  const double floor_budget =
    kFullBudget / static_cast<double>(1u << udp_bridge::kMaxAckStarvationBackoffShift);
  EXPECT_LE(rates.success_bytes_per_second, floor_budget)
    << "Deep ack starvation did not clamp the resend budget to the "
       "1/2^kMaxAckStarvationBackoffShift floor.";
  // floor(1562 / 1000) = 1 packet still goes out as the recovery probe.
  EXPECT_GT(rates.success_bytes_per_second, 0.0f)
    << "Max backoff shut resends off entirely — the floor is supposed "
       "to keep a probe trickle so recovery is possible the moment the "
       "inbound path returns.";
}

TEST_F(ResendBudget, ProbeFloorLowRateLink)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  std::vector<uint64_t> missing;
  auto conn = make_seeded_connection(t0, &missing);
  // Default-class link: 50 kB/s. At max backoff the floored budget is
  // 50000 * 0.25 / 16 = 781 bytes — smaller than one 1000-byte packet.
  // The probe guarantee must still admit exactly one packet per window
  // rather than shedding everything (pre-push review suggestion 1).
  conn->setRateLimit(50000);
  conn->update_last_receive_time(
    t0.seconds() - 100.0 * udp_bridge::kAckStarvationThreshold.count(), 100, false);

  conn->resend_packets(missing, send_sock_.get(), t0);

  auto rates = conn->data_sent_rate(t0, udp_bridge::PacketSendCategory::resend);
  EXPECT_EQ(rates.success_bytes_per_second, static_cast<float>(kPacketSize))
    << "Sub-packet floored budget must admit exactly one probe packet "
       "per window — zero means the probe trickle silently vanished on "
       "low-rate links; more means the budget bound broke.";
}

TEST_F(ResendBudget, ZeroFractionDisablesResends)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  std::vector<uint64_t> missing;
  auto conn = make_seeded_connection(t0, &missing);
  conn->setResendBudgetFraction(0.0f);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  conn->resend_packets(missing, send_sock_.get(), t0);

  auto rates = conn->data_sent_rate(t0, udp_bridge::PacketSendCategory::resend);
  // fraction 0.0 is the operator's "no resends on this connection"
  // switch — the probe guarantee must not override it.
  EXPECT_EQ(rates.success_bytes_per_second, 0.0f)
    << "fraction 0.0 must disable resends entirely (probe guarantee "
       "leaked past a zero budget).";
}

TEST_F(ResendBudget, NanFractionFallsBackToDefault)
{
  udp_bridge::Connection conn("nan-test", "127.0.0.1", 1, "", 0);
  conn.setResendBudgetFraction(std::nanf(""));
  // NaN must map to the default, not slip through std::min/max to the
  // most-permissive 1.0 (pre-push review suggestion 2).
  EXPECT_EQ(conn.resendBudgetFraction(), udp_bridge::kDefaultResendBudgetFraction);
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

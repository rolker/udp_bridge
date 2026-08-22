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
//   DecreaseTargetsHeadroomOfGoodput — a congested sample drops the cap to
//                                 (1 - link_headroom_fraction) * goodput,
//                                 not merely half of where it was (#52).
//   DuplicatesExcludedFromGoodput — duplicate bytes are subtracted before
//                                 the headroom target is computed (#52).
//   CongestionDetectionUsesRawReceivedNotGoodput — the congested predicate
//                                 compares raw received vs sent (both
//                                 include resends), not goodput (#52).
//   AdditiveRecoveryIsRelativeToEffectiveCap — a clean sample recovers a
//                                 step relative to the CURRENT cap, never
//                                 a fraction of the configured limit (#52).
//   FloorIsAbsoluteNotCapRelative — repeated congestion clamps at
//                                 kDefaultAdmissionFloorBytesPerSecond,
//                                 independent of the configured limit (#52).
//   FloorNeverExceedsConfiguredLimit — a floor above a connection's own
//                                 limit yields the limit, not a raise (#52).
//   NegativeFloorFallsBackToDefault — a negative/NaN floor falls back to
//                                 the default, never clamps to 0 (#52).
//   CeilingClamp                — recovery never exceeds the configured
//                                 limit.
//   FeedbackStaleBackoff        — stale last_receive_time is congestion
//                                 even when the reported delivery is high.
//   NanFeedbackTreatedAsCongestion — a non-finite delivery report is
//                                 unusable feedback: congestion + plain
//                                 halving, never read as clean (#52).
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

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
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

TEST_F(AdmissionControl, DecreaseTargetsHeadroomOfGoodput)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);                   // sent ~80 kB/s
  conn->update_last_receive_time(t0.seconds(), 100, false);  // feedback fresh

  // Remote reports 20 kB/s delivered against 80 kB/s sent — congested.
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);

  // The cap goes to the headroom target, not to half of where it was.
  // Half of 100000 is 50000, which is still 2.5x what the link is
  // actually carrying — the #52 failure mode in miniature.
  const uint32_t expected = static_cast<uint32_t>(
    (1.0f - udp_bridge::kDefaultLinkHeadroomFraction) * 20000.0f);
  EXPECT_EQ(conn->effectiveRateLimit(), expected)
    << "A congested sample must target (1 - headroom) x goodput, so the "
       "cap converges on what the link delivers instead of walking down "
       "from a configured number that does not describe it.";
  EXPECT_LT(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "Regression guard: a plain halving would leave the cap far above "
       "measured delivery.";
}

TEST_F(AdmissionControl, DuplicatesExcludedFromGoodput)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  // Same reported received rate; one connection's traffic is mostly
  // duplicates. received_bytes_per_second counts resends and duplicates,
  // so it flatters the link exactly when amplification is worst.
  auto clean = make_connection();
  send_traffic(*clean, t0, 80);
  clean->update_last_receive_time(t0.seconds(), 100, false);
  clean->updateAdmissionControl(40000.0f, 0.0f, t0);

  auto churning = make_connection();
  send_traffic(*churning, t0, 80);
  churning->update_last_receive_time(t0.seconds(), 100, false);
  churning->updateAdmissionControl(40000.0f, 30000.0f, t0);

  EXPECT_EQ(clean->goodputBytesPerSecond(), 40000.0f);
  EXPECT_EQ(churning->goodputBytesPerSecond(), 10000.0f);
  EXPECT_LT(churning->effectiveRateLimit(), clean->effectiveRateLimit())
    << "Duplicate bytes must be subtracted before the headroom target is "
       "computed — otherwise resend churn reads as healthy delivery.";
}

TEST_F(AdmissionControl, CongestionDetectionUsesRawReceivedNotGoodput)
{
  // A healthy link that delivers everything we sent but also reports a
  // large duplicate rate (reordering / a resend that raced its original).
  // Detection must compare RAW received against sent, so this reads as
  // clean and the cap stays put. If detection used goodput (received −
  // duplicates) while sent still included our resends, the ratio would
  // read as congested and throttle a link that lost nothing (#52).
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);                   // sent ~80 kB/s
  conn->update_last_receive_time(t0.seconds(), 100, false);

  // received == sent (nothing lost), but 30 kB/s of it is duplicates, so
  // goodput is only 50 kB/s — below the (1 - loss) x sent threshold.
  conn->updateAdmissionControl(80000.0f, 30000.0f, t0);

  EXPECT_EQ(conn->goodputBytesPerSecond(), 50000.0f);
  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit)
    << "Congestion DETECTION must use raw received vs sent (both include "
       "our resends, so the inflation cancels). A goodput-based predicate "
       "would falsely throttle a lossless-but-duplicating link.";
}

TEST_F(AdmissionControl, AdditiveRecoveryIsRelativeToEffectiveCap)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  for(int i = 0; i < 10; ++i)
    conn->updateAdmissionControl(0.0f, 0.0f, t0);      // drive to the floor
  const uint32_t floor =
    static_cast<uint32_t>(udp_bridge::kDefaultAdmissionFloorBytesPerSecond);
  ASSERT_EQ(conn->effectiveRateLimit(), floor);

  conn->updateAdmissionControl(80000.0f, 0.0f, t0);    // clean (100% delivered)

  const float step = std::max(
    udp_bridge::kAdmissionAdditiveStepMinimumBytesPerSecond,
    udp_bridge::kAdmissionAdditiveStepFraction *
      udp_bridge::kDefaultAdmissionFloorBytesPerSecond);
  EXPECT_EQ(conn->effectiveRateLimit(), static_cast<uint32_t>(floor + step))
    << "Recovery must step relative to the CURRENT effective cap.";
  EXPECT_LT(conn->effectiveRateLimit(),
            static_cast<uint32_t>(
              floor + udp_bridge::kAdmissionAdditiveStepFraction * kRateLimit))
    << "Regression guard for #52: a cap-relative step would add 10% of the "
       "configured limit here, which on a collapsed link is many times the "
       "whole path and undoes several decreases in one sample.";
}

TEST_F(AdmissionControl, FloorIsAbsoluteNotCapRelative)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  const uint32_t floor =
    static_cast<uint32_t>(udp_bridge::kDefaultAdmissionFloorBytesPerSecond);

  auto conn = make_connection();
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  for(int i = 0; i < 10; ++i)
    conn->updateAdmissionControl(0.0f, 0.0f, t0);      // total loss, repeatedly

  EXPECT_EQ(conn->effectiveRateLimit(), floor)
    << "Repeated congestion must clamp at the absolute admission floor.";

  // The same floor on a connection configured ten times larger. Under
  // #43 the floor was a fraction of the configured limit, so this
  // connection would have floored ten times higher — on a degraded link
  // that meant "backing off to the floor" still offered several times
  // what the path could carry.
  auto big = std::make_unique<udp_bridge::Connection>(
    "admission-test-big", "127.0.0.1", listener_port_, "", 0);
  big->setRateLimit(kRateLimit * 10);
  send_traffic(*big, t0, 80);
  big->update_last_receive_time(t0.seconds(), 100, false);
  for(int i = 0; i < 20; ++i)
    big->updateAdmissionControl(0.0f, 0.0f, t0);

  EXPECT_EQ(big->effectiveRateLimit(), floor)
    << "The floor must not scale with the configured limit (#52).";
}

TEST_F(AdmissionControl, FloorNeverExceedsConfiguredLimit)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  // A connection whose configured limit is below the default floor.
  const uint32_t small_limit =
    static_cast<uint32_t>(udp_bridge::kDefaultAdmissionFloorBytesPerSecond) / 2;
  auto conn = std::make_unique<udp_bridge::Connection>(
    "admission-test-small", "127.0.0.1", listener_port_, "", 0);
  conn->setRateLimit(small_limit);
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  for(int i = 0; i < 10; ++i)
    conn->updateAdmissionControl(0.0f, 0.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(), small_limit)
    << "A floor above the connection's own limit must yield the limit — "
       "the floor is a lower bound on backoff, never a way to raise the cap.";
}

TEST_F(AdmissionControl, NegativeFloorFallsBackToDefault)
{
  // Contract (connection.h): negative and NaN floors fall back to the
  // default. A negative value must NOT clamp to 0 — that would disable
  // the lockout floor and remove the control/feedback protection.
  auto conn = make_connection();
  conn->setAdmissionFloorBytesPerSecond(-1.0f);
  EXPECT_EQ(conn->admissionFloorBytesPerSecond(),
            udp_bridge::kDefaultAdmissionFloorBytesPerSecond)
    << "A negative admission floor must fall back to the default, not "
       "clamp to 0 (which disables the floor).";

  conn->setAdmissionFloorBytesPerSecond(
    std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(conn->admissionFloorBytesPerSecond(),
            udp_bridge::kDefaultAdmissionFloorBytesPerSecond)
    << "A NaN admission floor must fall back to the default.";
}

TEST_F(AdmissionControl, CeilingClamp)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  // Recovery is relative to the current cap now, so reaching the ceiling
  // takes more clean samples than the old cap-relative step did.
  for(int i = 0; i < 60; ++i)
    conn->updateAdmissionControl(80000.0f, 0.0f, t0);  // clean, repeatedly

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
  conn->updateAdmissionControl(10000.0f, 0.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "Stale feedback (nothing received for kAckStarvationThreshold) "
       "must count as congestion regardless of the reported delivery, and "
       "must fall back to a plain multiplicative decrease — the headroom "
       "target needs a goodput reading, which is exactly what stale means "
       "we do not have (#52).";
}

TEST_F(AdmissionControl, NanFeedbackTreatedAsCongestion)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);                    // sent, feedback fresh
  conn->update_last_receive_time(t0.seconds(), 100, false);

  // A NaN delivery report must not read as a clean sample: NaN fails the
  // < comparison, so left untreated it would silently defeat the AIMD
  // decrease. It is unusable feedback — count it as congestion and fall
  // back to a plain halving (no trustworthy goodput to target) (#52).
  conn->updateAdmissionControl(std::numeric_limits<float>::quiet_NaN(),
                               0.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "Non-finite remote feedback must count as congestion and apply the "
       "multiplicative decrease, not be silently treated as clean (#52).";
}

TEST_F(AdmissionControl, NeverReceivedSentinel)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  // No traffic sent, nothing ever received (0.0 sentinel): an idle,
  // brand-new connection is NOT congested — the cap stays at the
  // configured limit (additive step is ceiling-clamped).
  conn->updateAdmissionControl(0.0f, 0.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit)
    << "last_receive_time 0.0 (never received) must not read as stale "
       "feedback — new connections start at the full cap.";
}

TEST_F(AdmissionControl, EffectiveLimitAffectsCanSend)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  // Real congestion path (the idle guard means stale feedback alone,
  // with nothing sent, is deliberately NOT congestion): seed 10 kB of
  // sent traffic, then take the stale-feedback branch. Stale is used
  // here rather than a low delivery report because it is the one
  // congested path that does NOT apply the headroom target (there is no
  // trustworthy goodput reading when feedback is stale), so it yields a
  // plain halving and keeps this test's arithmetic about can_send
  // rather than about the target calculation.
  send_traffic(*conn, t0);                       // 10 kB @ t0
  conn->update_last_receive_time(
    t0.seconds() - 2.0 * udp_bridge::kAckStarvationThreshold.count(), 100, false);
  conn->updateAdmissionControl(10000.0f, 0.0f, t0);   // halve to 50000
  ASSERT_EQ(conn->effectiveRateLimit(), kRateLimit / 2);

  // Offer 2x the configured limit in a fresh 1-second window: can_send
  // admits strictly under the REDUCED cap there (~49 kB), while the
  // t0 seed sits outside that window.
  const auto t1 = t0 + rclcpp::Duration::from_seconds(2.0);
  std::vector<uint8_t> data(kPacketSize, 0xEF);
  for(int i = 0; i < 200; ++i)                   // 200 kB offered
    conn->send(data, send_sock_.get(), udp_bridge::PacketSendCategory::message, t1);

  // data_sent_rate spans t0..t1 (2 s), so expected success is
  // (10 kB seed + <50 kB admitted) / ~2 s ≈ 29.5 kB/s. If send() were
  // still metering at the CONFIGURED cap, t1 would admit ~99 kB and
  // the rate would be ~54.5 kB/s — the 50 kB/s bound discriminates.
  auto rates = conn->data_sent_rate(t1, udp_bridge::PacketSendCategory::message);
  EXPECT_LT(rates.success_bytes_per_second, static_cast<float>(kRateLimit / 2))
    << "send() must meter against the AIMD-reduced effective cap "
       "(a full-cap limiter would show ~54.5 kB/s here).";
  EXPECT_GE(rates.success_bytes_per_second, 20000.0f)
    << "Far below the expected ~29.5 kB/s — over-throttling regression.";
  EXPECT_GT(rates.dropped_bytes_per_second, 0.0f)
    << "Offered load above the reduced cap must produce drops.";
}

TEST_F(AdmissionControl, IdleStaleLinkNotThrottled)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  // Dormant connection: nothing sent, and the last inbound packet is
  // long past the starvation threshold. Stale feedback on an IDLE link
  // must not read as congestion — no traffic was offered, so there is
  // nothing the backoff could protect, and accumulated backoff would
  // only penalize the link when traffic resumes.
  conn->update_last_receive_time(
    t0.seconds() - 10.0 * udp_bridge::kAckStarvationThreshold.count(), 100, false);
  conn->updateAdmissionControl(0.0f, 0.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit)
    << "An idle link with stale feedback must not accumulate backoff.";
}

TEST_F(AdmissionControl, SetRateLimitClampPreservesBackoff)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0);
  // Stale feedback: the congested branch that applies a plain halving,
  // giving this test a deterministic reduced cap to flap against.
  conn->update_last_receive_time(
    t0.seconds() - 2.0 * udp_bridge::kAckStarvationThreshold.count(), 100, false);
  conn->updateAdmissionControl(10000.0f, 0.0f, t0);   // halve to 50000
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

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
//   DecreaseIsPurelyMultiplicativeFromCurrentCap — a congested sample
//                                 halves the CURRENT cap and reads no
//                                 goodput term at all. The
//                                 (1 - headroom) x goodput clamp was
//                                 removed on 2026-08-25: goodput is
//                                 depressed by the throttling the clamp
//                                 was computing (#52).
//   RefractoryFreezesDecreaseWithinWindow / ...RecoveryWithinWindow /
//   RefractoryEvaluatesNextSampleAfterWindow /
//   RefractoryGrowsWithinEpisodeAndResetsOnCleanSample /
//   RefractoryPeriodSetterContract — the one-decision-per-epoch gate
//                                 that stopped the 2026-08-25 cascade,
//                                 including that a CLEAN sample inside
//                                 the window gets no recovery either,
//                                 and that the configured period is
//                                 clamped at BOTH ends so no value
//                                 (+Inf included) can freeze the
//                                 controller for the life of the node
//                                 (#52).
//   RefractoryWindowGrowthIsBounded — the refractory window must
//                                 actually grow within an unresolved
//                                 episode, and the grown value must stay
//                                 inside the 10 s of history the
//                                 statistics deque retains. Asserted
//                                 against absolute values, not against
//                                 the constants themselves (#52).
//   RefractoryGateHoldsAtClockZero — a decrease recorded at clock time
//                                 0.0 still opens the window: sim time
//                                 before the first /clock, and bag
//                                 replay from t=0, must not silently run
//                                 the pre-#52 controller (#52).
//   WindowMatchedSendRateSurvivesABurst — the congestion detector
//                                 measures our send rate over the same
//                                 5 s window the remote measures its
//                                 receive rate over; a burst that the
//                                 remote reports smoothed no longer
//                                 reads as 80% loss (#52).
//   DuplicatesExcludedFromGoodput — duplicate bytes are subtracted from
//                                 the reported goodput figure (#52).
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
//   NegativeRatesTreatedAsCongestion — a negative received/duplicate pair
//                                 is unusable feedback: plain halving, never
//                                 a floor slam (slips past duplicate>received
//                                 yet is below any threshold) (#52, #53).
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

  // Step past the refractory window (issue #52) so the next sample is
  // evaluated rather than frozen. Tests that drive several decisions in
  // sequence must advance time; feeding N samples at one timestamp now
  // produces exactly ONE decision, which is the whole point of the gate.
  static rclcpp::Time past_refractory(rclcpp::Time t)
  {
    return t + rclcpp::Duration::from_seconds(
      udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds *
        udp_bridge::kAdmissionRefractoryMaximumMultiple + 0.5);
  }

  SocketFd listener_sock_;
  SocketFd send_sock_;
  uint16_t listener_port_ = 0;
};

TEST_F(AdmissionControl, DecreaseIsPurelyMultiplicativeFromCurrentCap)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);                   // sent ~80 kB/s
  conn->update_last_receive_time(t0.seconds(), 100, false);  // feedback fresh

  // Remote reports 20 kB/s delivered against 80 kB/s sent — congested.
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);

  // Exactly one multiplicative step from where the controller was. NOT
  // (1 - headroom) x goodput = 16000, which is what this used to assert:
  // that clamp was removed on 2026-08-25 (#52) because goodput is
  // depressed by the throttling the clamp was computing, so the target
  // fed back on itself. On the recorded field onset its first step alone
  // landed below the regression bound, at any refractory tuning.
  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "A congested sample must halve the CURRENT cap and read no goodput "
       "term at all — the controller backs off from its own last known-good "
       "state, not from a measurement its own gate contaminated (#52).";

  // The goodput reading itself is still computed and published — the
  // resend budget consumes it. It just no longer steers the decrease.
  EXPECT_EQ(conn->goodputBytesPerSecond(), 20000.0f);
}

// NOTE: HeadroomFractionDoesNotAffectDecreaseTarget and
// HeadroomFractionClampedToContract lived here. They pinned the
// inertness and the clamp contract of `link_headroom_fraction`, whose
// setter and Connection state were REMOVED in #52 — there is no longer
// an API to pin. The decrease's independence from goodput is covered by
// DecreaseIsPurelyMultiplicativeFromCurrentCap above; what remains of
// the parameter is a node-level declared tripwire, covered by
// RetiredHeadroomParameterStillConfigures in test_node_name_limits.cpp.

TEST_F(AdmissionControl, DuplicatesExcludedFromGoodput)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();

  // Same reported received rate; one connection's traffic is mostly
  // duplicates. received_bytes_per_second counts resends and duplicates,
  // so it flatters the link exactly when amplification is worst.
  //
  // Since the 2026-08-25 clamp removal the goodput figure no longer
  // steers the admission decrease — but it is still the basis the resend
  // budget is measured against (doc/resend_budget_design.md), and it is
  // still published per connection, so the duplicate subtraction remains
  // a live contract. What this test pins moved from "the cap lands lower"
  // to the quantity itself.
  auto clean = make_connection();
  send_traffic(*clean, t0, 80);
  clean->update_last_receive_time(t0.seconds(), 100, false);
  clean->updateAdmissionControl(40000.0f, 0.0f, t0);

  auto churning = make_connection();
  send_traffic(*churning, t0, 80);
  churning->update_last_receive_time(t0.seconds(), 100, false);
  churning->updateAdmissionControl(40000.0f, 30000.0f, t0);

  EXPECT_EQ(clean->goodputBytesPerSecond(), 40000.0f);
  EXPECT_EQ(churning->goodputBytesPerSecond(), 10000.0f)
    << "Duplicate bytes must be subtracted from the reported received "
       "rate — otherwise resend churn reads as healthy delivery to every "
       "consumer of goodput, the resend budget included.";
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

  // Drive to the floor. Each decrease now costs a refractory window
  // (#52), so the samples have to be spaced — feeding ten at one
  // timestamp produces exactly one decision. Traffic is re-offered at
  // each step because an idle link (sent_bps == 0) can never be
  // congested.
  auto t = t0;
  const uint32_t floor =
    static_cast<uint32_t>(udp_bridge::kDefaultAdmissionFloorBytesPerSecond);
  for(int i = 0; i < 10; ++i)
  {
    send_traffic(*conn, t, 80);
    conn->update_last_receive_time(t.seconds(), 100, false);
    conn->updateAdmissionControl(0.0f, 0.0f, t);       // total loss
    t = past_refractory(t);
  }
  ASSERT_EQ(conn->effectiveRateLimit(), floor);

  send_traffic(*conn, t, 80);
  conn->update_last_receive_time(t.seconds(), 100, false);
  conn->updateAdmissionControl(80000.0f, 0.0f, t);     // clean (100% delivered)

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
  auto t = t0;
  for(int i = 0; i < 10; ++i)                          // total loss, repeatedly
  {
    send_traffic(*conn, t, 80);
    conn->update_last_receive_time(t.seconds(), 100, false);
    conn->updateAdmissionControl(0.0f, 0.0f, t);
    t = past_refractory(t);                            // one decision per epoch (#52)
  }

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
  auto tb = t0;
  for(int i = 0; i < 20; ++i)
  {
    send_traffic(*big, tb, 80);
    big->update_last_receive_time(tb.seconds(), 100, false);
    big->updateAdmissionControl(0.0f, 0.0f, tb);
    tb = past_refractory(tb);
  }

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

TEST_F(AdmissionControl, NanDuplicateFeedbackTreatedAsCongestion)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);                    // sent, feedback fresh
  conn->update_last_receive_time(t0.seconds(), 100, false);

  // received reads clean (== sent), but the DUPLICATE channel is NaN.
  // goodput = received − NaN collapses to 0; if that slipped through as a
  // usable sample the headroom target would slam the cap to the floor on
  // this one report. The duplicate channel must be validated like received:
  // count it as congestion and apply the plain halving (#52).
  conn->updateAdmissionControl(80000.0f,
                               std::numeric_limits<float>::quiet_NaN(), t0);

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "A non-finite duplicate report must count as congestion and apply "
       "the multiplicative decrease, not slam the cap to the floor (#52).";
}

TEST_F(AdmissionControl, DuplicateExceedingReceivedTreatedAsCongestion)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  // A duplicate rate above received is impossible physically (you cannot
  // duplicate more than you received) — a smoothing-window skew, or a
  // hostile report. goodput = received − duplicate goes negative and
  // clamps to 0; treated as a usable sample the headroom target would
  // slam the cap to the floor. It is unusable feedback: plain halving.
  conn->updateAdmissionControl(40000.0f, 60000.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "A duplicate rate exceeding received must count as congestion and "
       "apply the multiplicative decrease, not slam the cap to the floor "
       "(#52).";
}

TEST_F(AdmissionControl, NegativeDuplicateDoesNotInflateGoodput)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  // A peer reporting a negative duplicate rate would inflate
  // received − duplicate above received (40000 − (−30000) = 70000). The
  // stored goodput must be clamped to what was actually received so the
  // resend-budget basis downstream cannot be sized against a fabricated
  // number (#52).
  conn->updateAdmissionControl(40000.0f, -30000.0f, t0);

  EXPECT_EQ(conn->goodputBytesPerSecond(), 40000.0f)
    << "Stored goodput must clamp to received; a negative duplicate rate "
       "must not inflate it above what the remote reported receiving (#52).";
}

TEST_F(AdmissionControl, NegativeRatesTreatedAsCongestion)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);

  // A negative received/duplicate pair is nonsensical (a rate cannot be
  // below zero) and reaches us over an unauthenticated transport (#53). It
  // slips past the finite and duplicate>received guards — with received =
  // −100, duplicate = −200 the check duplicate > received is FALSE — yet a
  // negative received is below any congestion threshold, so without the
  // negative guard the sample would be treated as usable-and-congested with
  // a headroom target of 0 and slam the cap to the floor in one report.
  // Reject negatives as unusable feedback: plain halving, not a floor slam.
  conn->updateAdmissionControl(-100.0f, -200.0f, t0);

  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "A negative received/duplicate pair must count as unusable feedback "
       "and apply the multiplicative decrease, not slam the cap to the floor "
       "(#52).";
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

// ---------------------------------------------------------------------
// Refractory period — one admission decision per congestion epoch (#52).
//
// The 2026-08-25 Appledore RCA: updateAdmissionControl runs on every
// BridgeInfo arrival (~2 s), and before this gate existed every arriving
// congested sample applied another halving. One marginal sample became
// seven halvings in 12 s — a 183x cap reduction, 35 times in 9.5 hours,
// while the link itself reported 0.0-0.05% drop throughout.
// ---------------------------------------------------------------------

TEST_F(AdmissionControl, RefractoryFreezesDecreaseWithinWindow)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();

  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);          // congested
  const uint32_t after_first = conn->effectiveRateLimit();
  ASSERT_EQ(after_first,
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor));

  // A second congested sample two seconds later — well inside the
  // default 5 s window — must not move the cap at all.
  const auto t1 = t0 + rclcpp::Duration::from_seconds(2.0);
  send_traffic(*conn, t1, 80);
  conn->update_last_receive_time(t1.seconds(), 100, false);
  conn->updateAdmissionControl(20000.0f, 0.0f, t1);

  EXPECT_EQ(conn->effectiveRateLimit(), after_first)
    << "A congested sample inside the refractory window must not decrease "
       "again. Each decrease shrinks our own send rate, which widens the "
       "filter skew between the two ends, which re-triggers the detector — "
       "that is the cascade, and this gate is what breaks it.";
}

TEST_F(AdmissionControl, RefractoryFreezesRecoveryWithinWindow)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();

  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);          // congested
  const uint32_t after_first = conn->effectiveRateLimit();

  // A CLEAN sample inside the window gets no additive recovery either.
  // The freeze is unconditional on sample type: recovering here would
  // re-open the loop from the other side — recover, re-cross the
  // threshold on the next differently-filtered sample, halve again.
  const auto t1 = t0 + rclcpp::Duration::from_seconds(2.0);
  send_traffic(*conn, t1, 80);
  conn->update_last_receive_time(t1.seconds(), 100, false);
  conn->updateAdmissionControl(80000.0f, 0.0f, t1);          // clean

  EXPECT_EQ(conn->effectiveRateLimit(), after_first)
    << "The refractory freeze is unconditional on sample type — a clean "
       "sample inside the window must not recover either (#52).";
}

TEST_F(AdmissionControl, RefractoryEvaluatesNextSampleAfterWindow)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();

  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);
  const uint32_t after_first = conn->effectiveRateLimit();

  // Once the window elapses the next sample is evaluated normally — the
  // gate rate-limits the controller, it does not disable it.
  const auto t1 = t0 + rclcpp::Duration::from_seconds(
    udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds + 0.5);
  send_traffic(*conn, t1, 80);
  conn->update_last_receive_time(t1.seconds(), 100, false);
  conn->updateAdmissionControl(80000.0f, 0.0f, t1);          // clean

  EXPECT_GT(conn->effectiveRateLimit(), after_first)
    << "After the window elapses, a clean sample must recover normally.";
}

TEST_F(AdmissionControl, RefractoryGrowsWithinEpisodeAndResetsOnCleanSample)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  auto t = clock.now();
  auto conn = make_connection();
  // NOTE: `grown` is DERIVED from the constants, so this test pins the
  // growth MECHANISM (a second decrease grows the window; the grown value
  // is the one in force; a clean sample gives the growth back) and not
  // the constants' values — it stays green for any of them. The values
  // are pinned by RefractoryWindowGrowthIsBounded above (#52 review r1).
  const double base = udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds;
  const double grown = std::min(base * udp_bridge::kAdmissionRefractoryGrowthFactor,
                                base * udp_bridge::kAdmissionRefractoryMaximumMultiple);

  ASSERT_EQ(conn->currentAdmissionRefractoryWindowSeconds(), base);

  auto congested_sample = [&](rclcpp::Time when)
  {
    send_traffic(*conn, when, 80);
    conn->update_last_receive_time(when.seconds(), 100, false);
    conn->updateAdmissionControl(20000.0f, 0.0f, when);
  };

  congested_sample(t);                                        // decrease 1
  EXPECT_EQ(conn->currentAdmissionRefractoryWindowSeconds(), base);
  const uint32_t after_first = conn->effectiveRateLimit();

  t = t + rclcpp::Duration::from_seconds(base + 0.5);
  congested_sample(t);                                        // decrease 2
  const uint32_t after_second = conn->effectiveRateLimit();
  EXPECT_LT(after_second, after_first);
  EXPECT_EQ(conn->currentAdmissionRefractoryWindowSeconds(), grown)
    << "A second decrease inside an unresolved episode must grow the "
       "window — the recorded onset reads congested on the raw ratio for "
       "its full ~14 s, so a fixed window still permits a third and "
       "fourth halving in one episode (#52).";

  // Inside the GROWN window (past the base, short of the grown value):
  // still frozen.
  t = t + rclcpp::Duration::from_seconds(base + 0.5);
  ASSERT_LT(base + 0.5, grown);
  congested_sample(t);
  EXPECT_EQ(conn->effectiveRateLimit(), after_second)
    << "The grown window must be the one in force, not the base.";

  // Past the grown window, a clean sample resolves the episode: it
  // recovers AND gives back the accumulated growth, so the next episode
  // starts from the base rather than inheriting this one's ceiling.
  t = t + rclcpp::Duration::from_seconds(grown + 0.5);
  send_traffic(*conn, t, 80);
  conn->update_last_receive_time(t.seconds(), 100, false);
  conn->updateAdmissionControl(80000.0f, 0.0f, t);            // clean
  EXPECT_GT(conn->effectiveRateLimit(), after_second);
  EXPECT_EQ(conn->currentAdmissionRefractoryWindowSeconds(), base)
    << "The first clean sample after a decrease is what 'the episode "
       "resolved' means — reset the window to its base (#52).";
}

TEST_F(AdmissionControl, RefractoryWindowGrowthIsBounded)
{
  // RefractoryGrowsWithinEpisodeAndResetsOnCleanSample derives its
  // expectation from kAdmissionRefractoryGrowthFactor and
  // kAdmissionRefractoryMaximumMultiple, so it is green for ANY value of
  // them — including 1.0 (no growth) and 1000 (a 5000 s freeze ceiling),
  // both verified empirically in #52 review round 1. The field-replay
  // suite is green for both too. Nothing pinned these numbers.
  //
  // This test asserts the two properties the constants exist to provide,
  // against absolute values taken from their own rationale rather than
  // from the symbols:
  //
  //  - the window must actually GROW within an unresolved episode. The
  //    recorded 07:20 onset reads congested on the raw ratio for its
  //    full ~14 s; a fixed window still permits a third and fourth
  //    halving inside that one episode.
  //  - the grown window must stay within the 10 s of history the
  //    statistics deque retains (Statistics::add) and the 10 s the
  //    range-degradation bench holds each phase for. Freezing longer
  //    than the whole measurement record means the controller's next
  //    decision rests on evidence it can no longer see.
  constexpr double kStatisticsRetentionSeconds = 10.0;

  rclcpp::Clock clock(RCL_STEADY_TIME);
  auto t = clock.now();
  auto conn = make_connection();
  const double base = udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds;

  // Report half of whatever the gate actually let through, so the sample
  // stays congested however far the cap has fallen — including once it
  // is pinned at the floor. A fixed reported rate would go CLEAN as soon
  // as the cap dropped below it, resolving the episode and resetting the
  // window before the growth had saturated.
  auto congested_sample = [&](rclcpp::Time when)
  {
    send_traffic(*conn, when, 80);
    conn->update_last_receive_time(when.seconds(), 100, false);
    const float admitted = static_cast<float>(conn->effectiveRateLimit());
    conn->updateAdmissionControl(admitted * 0.5f, 0.0f, when);
  };

  // Saturate the growth: step just past the window in force each time,
  // so every sample lands as a further decrease in one unresolved
  // episode. Ten rounds is far more than any sane multiple needs.
  congested_sample(t);
  double window = conn->currentAdmissionRefractoryWindowSeconds();
  for(int i = 0; i < 10; ++i)
  {
    t = t + rclcpp::Duration::from_seconds(
      conn->currentAdmissionRefractoryWindowSeconds() + 0.25);
    congested_sample(t);
    window = std::max(window, conn->currentAdmissionRefractoryWindowSeconds());
  }

  EXPECT_GT(window, base)
    << "The refractory window never grew (" << window << " s at the base "
    << base << " s). A fixed window still permits a third and fourth "
       "halving inside the ~14 s the recorded onset reads congested for "
       "(#52).";
  EXPECT_LE(window, kStatisticsRetentionSeconds)
    << "The grown refractory window reached " << window << " s, past the "
    << kStatisticsRetentionSeconds << " s of history the statistics deque "
       "retains and the bench holds each phase for. A freeze longer than "
       "the whole measurement record decides on evidence the controller "
       "can no longer see (#52).";
}

TEST_F(AdmissionControl, RefractoryGateHoldsAtClockZero)
{
  // The gate must not depend on the clock's ORIGIN. "A decrease is
  // outstanding" cannot be encoded as `last_admission_decrease_time_ >
  // 0.0`, because 0.0 is a legal clock reading: a decrease recorded at
  // exactly t=0 then reads as "no decrease outstanding", the window
  // never applies, and every following congested sample halves again —
  // the pre-#52 cascade, silently reintroduced.
  //
  // Reaching t=0 with a non-empty send history needs history stamped at
  // or before 0, and Statistics::add drops records timestamped exactly
  // 0 — so the traffic here is stamped BEFORE the origin.
  //
  // This test used to stamp the traffic at t=2 and then evaluate at t=0,
  // relying on success_rate_in_window summing those now-future samples.
  // That was a bug in the window, not a scenario: it is fixed (the
  // window is bounded above as well as below, #52 round 2), and a
  // backwards clock step now correctly reports NO send history until the
  // window refills. Keeping the old setup would have left this test
  // green for the wrong reason.
  //
  // What the test pins is unchanged and is the durable property: the
  // gate must not encode "a decrease is outstanding" as
  // `last_admission_decrease_time_ > 0.0`, because 0.0 is a clock
  // reading. Constructing a decrease recorded at exactly 0.0 is what the
  // pre-origin timestamps are for.
  auto conn = make_connection();
  const rclcpp::Time before_reset(
    -2 * static_cast<int64_t>(1000000000), RCL_ROS_TIME);
  send_traffic(*conn, before_reset, 80);

  const rclcpp::Time t0(0, 0, RCL_ROS_TIME);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);           // decrease 1
  const uint32_t after_first = conn->effectiveRateLimit();
  ASSERT_LT(after_first, kRateLimit)
    << "Setup: the sample at t=0 must itself be congested, or this test "
       "pins nothing.";

  // Well inside the refractory window, still at a clock time near zero.
  const auto t1 = t0 + rclcpp::Duration::from_seconds(
    udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds * 0.25);
  conn->update_last_receive_time(t1.seconds(), 100, false);
  conn->updateAdmissionControl(20000.0f, 0.0f, t1);

  EXPECT_EQ(conn->effectiveRateLimit(), after_first)
    << "A decrease recorded at clock time 0.0 must still open the "
       "refractory window — 0.0 is a clock value, not a sentinel (#52).";
}

TEST_F(AdmissionControl, RefractoryPeriodSetterContract)
{
  auto conn = make_connection();
  EXPECT_EQ(conn->admissionRefractoryPeriodSeconds(),
            udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds);

  conn->setAdmissionRefractoryPeriodSeconds(3.0);
  EXPECT_EQ(conn->admissionRefractoryPeriodSeconds(), 3.0);
  EXPECT_EQ(conn->currentAdmissionRefractoryWindowSeconds(), 3.0)
    << "Setting the base must also reset the in-force window, or a "
       "reconfiguration made while a grown window is open leaves the "
       "controller frozen under the value it was just told to abandon.";

  // NaN and negatives fall back to the default, matching the other
  // admission parameters' contract — a garbage value must degrade to the
  // safe default, never to "gate off".
  conn->setAdmissionRefractoryPeriodSeconds(
    std::numeric_limits<double>::quiet_NaN());
  EXPECT_EQ(conn->admissionRefractoryPeriodSeconds(),
            udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds);
  conn->setAdmissionRefractoryPeriodSeconds(-1.0);
  EXPECT_EQ(conn->admissionRefractoryPeriodSeconds(),
            udp_bridge::kDefaultAdmissionRefractoryPeriodSeconds);

  // 0 is a legitimate value and means "no gate": every sample acts. It is
  // reachable on purpose — reproducing the 2026-08-25 collapse needs it —
  // but only by asking for it explicitly.
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  conn->setAdmissionRefractoryPeriodSeconds(0.0);
  EXPECT_EQ(conn->admissionRefractoryPeriodSeconds(), 0.0);
  send_traffic(*conn, t0, 80);
  conn->update_last_receive_time(t0.seconds(), 100, false);
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);
  conn->updateAdmissionControl(20000.0f, 0.0f, t0);
  EXPECT_EQ(conn->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit *
                                  udp_bridge::kAdmissionDecreaseFactor *
                                  udp_bridge::kAdmissionDecreaseFactor))
    << "With the gate disabled, two congested samples must apply two "
       "decreases — the pre-#52 behaviour, available deliberately.";

  // The TOP end is clamped too. +Inf — or a large finite typo — used to
  // be accepted verbatim, and one congested sample then froze the
  // controller for the life of the node: no decrease, no recovery, and
  // nothing logged. Clamping (not falling back to the default) keeps an
  // operator's intent to slow the loop down while keeping it alive.
  conn->setAdmissionRefractoryPeriodSeconds(
    std::numeric_limits<double>::infinity());
  EXPECT_EQ(conn->admissionRefractoryPeriodSeconds(),
            udp_bridge::kMaximumAdmissionRefractoryPeriodSeconds);
  EXPECT_EQ(conn->currentAdmissionRefractoryWindowSeconds(),
            udp_bridge::kMaximumAdmissionRefractoryPeriodSeconds);
  conn->setAdmissionRefractoryPeriodSeconds(
    udp_bridge::kMaximumAdmissionRefractoryPeriodSeconds * 1000.0);
  EXPECT_EQ(conn->admissionRefractoryPeriodSeconds(),
            udp_bridge::kMaximumAdmissionRefractoryPeriodSeconds);

  // And the clamped window really does expire: the freeze is bounded in
  // time, not just in the stored number.
  auto frozen = make_connection();
  frozen->setAdmissionRefractoryPeriodSeconds(
    std::numeric_limits<double>::infinity());
  const auto t1 = t0 + rclcpp::Duration::from_seconds(1.0);
  send_traffic(*frozen, t1, 80);
  frozen->update_last_receive_time(t1.seconds(), 100, false);
  frozen->updateAdmissionControl(20000.0f, 0.0f, t1);
  const uint32_t after_freeze = frozen->effectiveRateLimit();
  ASSERT_LT(after_freeze, kRateLimit);

  // GROWTH does not escape the ceiling. A second decrease inside the
  // still-open window multiplies the current window by the growth factor
  // and bounds it by base x kAdmissionRefractoryMaximumMultiple — which
  // at the maximum base is 120 s, twice the ceiling. The ceiling has to
  // be re-applied after growth, or a max-configured connection freezes
  // for longer than the bound whose rationale forbids it (#52, round 2).
  const auto t_grow = t1 + rclcpp::Duration::from_seconds(
    udp_bridge::kMaximumAdmissionRefractoryPeriodSeconds + 1.0);
  send_traffic(*frozen, t_grow, 80);
  frozen->update_last_receive_time(t_grow.seconds(), 100, false);
  frozen->updateAdmissionControl(20000.0f, 0.0f, t_grow);     // decrease 2
  EXPECT_LE(frozen->currentAdmissionRefractoryWindowSeconds(),
            udp_bridge::kMaximumAdmissionRefractoryPeriodSeconds)
    << "The GROWN window must be re-clamped to "
       "kMaximumAdmissionRefractoryPeriodSeconds. Bounding growth only "
       "relative to the base lets a max-configured connection freeze for "
       "base x multiple — 120 s — which is twice the ceiling the base "
       "clamp exists to enforce.";
  const uint32_t after_second = frozen->effectiveRateLimit();
  ASSERT_LT(after_second, after_freeze);

  const auto t2 = t_grow + rclcpp::Duration::from_seconds(
    udp_bridge::kMaximumAdmissionRefractoryPeriodSeconds + 1.0);
  send_traffic(*frozen, t2, 80);
  frozen->update_last_receive_time(t2.seconds(), 100, false);
  frozen->updateAdmissionControl(80000.0f, 0.0f, t2);         // clean
  EXPECT_GT(frozen->effectiveRateLimit(), after_second)
    << "An +Inf refractory period must not outlive the clamp: the "
       "controller has to recover once the clamped window elapses.";
}

TEST_F(AdmissionControl, WindowMatchedSendRateSurvivesABurst)
{
  // The detector compares what the remote reports receiving against what
  // we sent. The remote's figure comes from a 5 s box filter
  // (Connection::data_receive_rate), so ours must be measured over the
  // same 5 s window (#52). It used to come from
  // PacketSendStatistics::get(), a variable 1-10 s span, and the mismatch
  // is what made 16.1% of the 2026-08-25 field samples report the remote
  // receiving MORE than we sent — physically impossible if both described
  // the same interval.
  //
  // Concretely: a 90 kB burst at t0 and nothing after. Five seconds later
  // the remote honestly reports 18 kB/s — the burst, smoothed over its
  // window. get() would have reported 90000 B/s for the same traffic
  // (its deque spans a single timestamp, so no division happens), making
  // an honest report look like 80% loss.
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto conn = make_connection();

  send_traffic(*conn, t0, 90);                     // 90 kB, all at t0
  const auto t1 = t0 + rclcpp::Duration::from_seconds(
    udp_bridge::kAdmissionSendRateWindowSeconds);
  conn->update_last_receive_time(t1.seconds(), 100, false);
  conn->updateAdmissionControl(18000.0f, 0.0f, t1);   // 90000 / 5 s

  EXPECT_EQ(conn->effectiveRateLimit(), kRateLimit)
    << "A burst the remote reports smoothed over its own 5 s window must "
       "not read as congestion once our side is measured over the same "
       "window (#52).";

  // Two-sided: the same shape with genuinely less delivered still trips
  // the detector. The window match narrows the skew; it does not make
  // the controller deaf.
  auto lossy = make_connection();
  send_traffic(*lossy, t0, 90);
  lossy->update_last_receive_time(t1.seconds(), 100, false);
  lossy->updateAdmissionControl(9000.0f, 0.0f, t1);   // half of it lost

  EXPECT_EQ(lossy->effectiveRateLimit(),
            static_cast<uint32_t>(kRateLimit * udp_bridge::kAdmissionDecreaseFactor))
    << "Real loss measured over the matched window must still decrease.";
}

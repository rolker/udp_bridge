// Regression tests reproducing the 2026-08-25 Appledore link collapse
// (issue #52).
//
// FIELD EVIDENCE these pin. On the Isles of Shoals deployment
// (rolker/unh_echoboats_project11#459) the vpn admission cap collapsed to
// its floor 35 times across 9.5 hours -- 15 on the transit, 20 on station.
// Every episode bottomed at exactly 8192 B/s and every one lasted 102-110 s,
// a constancy that identifies the outage length as a property of THIS
// CONTROLLER rather than of the link. 1.95 GB of video and telemetry was
// discarded by the boat at its own gate before ever reaching the radio,
// while Starlink reported pop_ping_drop_rate 0.0-0.05, no obstruction and
// flat 17-24 ms latency throughout. Whenever the cap was left at maximum
// -- 80% of the transit, 90% on station -- median and p95 drops were ZERO.
//
// The trigger was ~1.11% loss. The response was a 183x cap reduction.
//
// Three tests, deliberately at different levels:
//
//   FieldOnsetMustNotCascadeToFloor -- replays the RECORDED sender/receiver
//       telemetry from the 07:20 episode. Pins the narrow contract: one
//       marginal sample must not cascade into a run to the floor.
//
//   HandoverBlipMustNotCostTwoMinutesOfVideo -- a closed-loop model of the
//       day's actual operating point (offered load well under capacity,
//       ~1% loss, and the smoothing skew between the two ends that is the
//       real defect). Pins the property that matters operationally: the
//       operator keeps their video.
//
//   SustainedRealLossMustConverge -- the other side of the gate. A link
//       whose capacity genuinely drops and stays down; the controller
//       must still find it, and find it in seconds. Without this one, a
//       controller that has stopped detecting congestion at all passes
//       both tests above.
//
// All three are deterministic, need no network impairment, and fail
// loudly against the pre-fix controller.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
  SocketFd(SocketFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  SocketFd& operator=(SocketFd&& o) noexcept
  {
    if(this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
  }
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void reset() { if(fd_ >= 0) { close(fd_); fd_ = -1; } }
private:
  int fd_ = -1;
};

SocketFd bind_localhost_listener(uint16_t* port)
{
  SocketFd sock(socket(AF_INET, SOCK_DGRAM, 0));
  if(!sock.valid()) return sock;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if(bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    return SocketFd{};
  socklen_t len = sizeof(addr);
  if(getsockname(sock.get(), reinterpret_cast<sockaddr*>(&addr), &len) < 0)
    return SocketFd{};
  *port = ntohs(addr.sin_port);
  return sock;
}

// The boat's configured vpn cap on 2026-08-25 (bizzyboat.yaml).
constexpr uint32_t kFieldRateLimit = 1500000;
constexpr int kPacketSize = 1000;

// The feedback interval measured in the field: 5534 BridgeInfo messages
// over 10932 s of transit.
constexpr double kFeedbackInterval = 1.975;

struct FeedbackSample
{
  float sent_on_wire_bps;
  float remote_received_bps;
  float remote_duplicate_bps;
};

// Recorded from the vpn connection of the 2026-08-25 transit bag
// (~/data/logs/gabby/logs/bizzyboat/2026-08-25T10-56-30+00-00),
// 07:20:33 - 07:21:07 local. sent_on_wire is the boat's own
// message+overhead+resend success rate; the other two are pandy's echoed
// BridgeInfo for this connection, i.e. the exact inputs to
// updateAdmissionControl.
//
// Note rows 1-6: the link is healthy and the cap is at its 1500000
// maximum. Row 7 (07:20:45) is the single marginal sample that started
// the cascade -- 442300/513563 = 0.86 against the 0.9 threshold.
constexpr FeedbackSample kFieldOnset[] = {
  {   506216.0f,   515214.0f,   93474.0f},  // 07:20:33  healthy, at cap
  {   514376.0f,   513024.0f,   94068.0f},  // 07:20:35  healthy, at cap
  {   510881.0f,   517577.0f,   93887.0f},  // 07:20:38  healthy, at cap
  {   510659.0f,   500911.0f,   92275.0f},  // 07:20:39  healthy, at cap
  {   514728.0f,   502744.0f,   92391.0f},  // 07:20:41  healthy, at cap
  {   514795.0f,   500890.0f,   90939.0f},  // 07:20:43  healthy, at cap
  {   513563.0f,   442300.0f,   89320.0f},  // 07:20:45  <-- the one marginal sample
  {   467577.0f,   378269.0f,   70747.0f},  // 07:20:47  field cap fell to 282383
  {   413375.0f,   254615.0f,   51715.0f},  // 07:20:49  field cap fell to 141191
  {   312664.0f,   140528.0f,   39607.0f},  // 07:20:51  field cap fell to  70595
  {   231666.0f,    70586.0f,   22508.0f},  // 07:20:53  field cap fell to  35297
  {   146301.0f,    35464.0f,   14024.0f},  // 07:20:55  field cap fell to  17648
  {    73716.0f,    17071.0f,    8863.0f},  // 07:20:57  field cap fell to   8824
  {    38874.0f,     9389.0f,    5883.0f},  // 07:20:59  field cap hit FLOOR 8192
};

}  // namespace

class AdmissionFieldReplay : public ::testing::Test
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
      "field-replay", "127.0.0.1", listener_port_, "", 0);
    conn->setRateLimit(kFieldRateLimit);
    return conn;
  }

  // Offer `bytes_per_second` spread evenly across the interval ENDING at
  // t, rather than as one instantaneous burst.
  //
  // (A `send_at_rate` helper that stamped a whole second's worth of bytes
  // at a single instant lived here until #52 review round 3, when its
  // last caller moved to this one and it was removed rather than left as
  // dead code. Its bias is described below because it is what this
  // helper exists to avoid.)
  //
  // Bunching a second of traffic at one instant is adequate when a test
  // only needs SOME send history to exist. It is not adequate when a
  // test asserts where the controller CONVERGES, because the rate the
  // detector measures has to be the rate the test believes it is
  // offering. Bunched at one
  // timestamp, a 1.975 s cadence carrying 1 s of bytes reads ~25% low
  // over the 5 s measurement window, which would move the convergence
  // band for reasons that have nothing to do with the control law.
  void send_over_interval(udp_bridge::Connection& conn, rclcpp::Time t,
                          double interval, float bytes_per_second)
  {
    const int packets =
      std::max(0, static_cast<int>(bytes_per_second * interval) / kPacketSize);
    if(packets == 0)
      return;
    std::vector<uint8_t> data(kPacketSize, 0xCD);
    for(int i = 0; i < packets; ++i)
    {
      const double offset = interval * static_cast<double>(i + 1) / packets;
      conn.send(data, send_sock_.get(), udp_bridge::PacketSendCategory::message,
                t - rclcpp::Duration::from_seconds(interval - offset));
    }
  }

  SocketFd listener_sock_;
  SocketFd send_sock_;
  uint16_t listener_port_ = 0;
};

// Replay the recorded onset. The contract is NOT "never back off" -- a
// controller is entitled to react to the 07:20:45 sample. The contract is
// that reacting must not run away: a single congestion event must cost at
// most one decrease, not a slide to the floor over the following samples.
TEST_F(AdmissionFieldReplay, FieldOnsetMustNotCascadeToFloor)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  auto t = clock.now();
  auto conn = make_connection();

  uint32_t lowest = kFieldRateLimit;
  for(const auto& s: kFieldOnset)
  {
    // Spread the row's bytes across the feedback interval rather than
    // stamping them at one instant. Bunched, a 1.975 s cadence carrying
    // a second of bytes reads ~24% low over the detector's 5 s window,
    // which makes the 07:20:45 row -- the single marginal sample this
    // test is named for -- read CLEAN and moves the first decrease two
    // rows later. The test still discriminated, but not by the mechanism
    // its name, comment and failure message describe (#52 review r1).
    send_over_interval(*conn, t, kFeedbackInterval, s.sent_on_wire_bps);
    conn->update_last_receive_time(t.seconds(), 100, false);
    conn->updateAdmissionControl(s.remote_received_bps, s.remote_duplicate_bps, t);
    lowest = std::min(lowest, conn->effectiveRateLimit());
    t = t + rclcpp::Duration::from_seconds(kFeedbackInterval);
  }

  // The floor is where the field controller ended up, 12 s after a 1.11%
  // loss event. Landing there again means the cascade is intact.
  EXPECT_GT(lowest, udp_bridge::kDefaultAdmissionFloorBytesPerSecond)
    << "The recorded 2026-08-25 onset drove the cap to its floor. A single "
       "marginal delivery sample (442300/513563 = 0.86) must not cascade "
       "into a run to the floor: each decrease shrinks the send rate, which "
       "widens the smoothing skew between the two ends, which re-triggers "
       "the detector. Apply at most one decrease per feedback round-trip.";

  // One multiplicative decrease from the cap is 750000. Allow two for
  // slack, but nothing like the 183x reduction the field saw.
  EXPECT_GT(lowest, kFieldRateLimit / 8)
    << "A single congestion event cost the field 183x of cap and ~100 s of "
       "unusable video. At most one decrease per event should be visible "
       "here.";
}

// The day's actual operating point, closed-loop. Offered load is well
// under both the configured cap and the measured Starlink capacity, and
// the link drops ~1% -- which is simply what Starlink does, handing over
// every 15 s. The two ends measure with different smoothing, so the
// receiver's report lags the sender's own rate; that skew is modelled
// here because it is the real defect, not an artifact.
//
// A correct controller leaves the cap alone and the operator keeps their
// video. The pre-fix controller collapses and discards most of it.
TEST_F(AdmissionFieldReplay, HandoverBlipMustNotCostTwoMinutesOfVideo)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  auto t = clock.now();
  auto conn = make_connection();

  constexpr float kOfferedBps = 495000.0f;   // measured median offered load
  constexpr float kBaseLoss = 0.011f;        // measured 1.11% background loss
  constexpr int kSteps = 60;                 // ~2 minutes of feedback
  constexpr int kBlipStep = 10;              // one Starlink handover
  constexpr int kBlipLength = 2;             // ~4 s of elevated loss
  constexpr float kBlipLoss = 0.60f;

  double admitted_total = 0.0;
  double offered_total = 0.0;
  int decreases = 0;

  for(int i = 0; i < kSteps; ++i)
  {
    const float cap = static_cast<float>(conn->effectiveRateLimit());
    const float admitted = std::min(kOfferedBps, cap);
    // Spread over the feedback interval, not bunched at one instant.
    // A whole second of bytes stamped at a single timestamp reads ~24%
    // LOW over the detector's 5 s window — and here the
    // bias runs toward FEWER decreases, weakening EXPECT_LE(decreases, 1)
    // below, which is the assertion that actually pins the gate (#52,
    // review round 2). Same correction FieldOnsetMustNotCascadeToFloor
    // already carries.
    send_over_interval(*conn, t, kFeedbackInterval, admitted);
    conn->update_last_receive_time(t.seconds(), 100, false);

    const bool blip = (i >= kBlipStep && i < kBlipStep + kBlipLength);
    const float loss = blip ? kBlipLoss : kBaseLoss;
    conn->updateAdmissionControl(admitted * (1.0f - loss), 0.0f, t);

    if(conn->effectiveRateLimit() < static_cast<uint32_t>(cap))
      ++decreases;

    admitted_total += admitted;
    offered_total += kOfferedBps;
    t = t + rclcpp::Duration::from_seconds(kFeedbackInterval);
  }

  const double delivered_fraction = admitted_total / offered_total;
  EXPECT_GT(delivered_fraction, 0.90)
    << "A single ~4 s loss blip -- which is simply what a Starlink handover "
       "looks like, and they happen every 15 s by design -- left the gate "
       "admitting only "
    << (100.0 * delivered_fraction) << "% of offered traffic over the "
       "following two minutes. On 2026-08-25 this cost 23.4% of message "
       "bytes on the transit and left the operator hand-driving the boat "
       "alongside with no RGB video. The recovery ramp (+2048 B/s per "
       "interval, then x1.1) means every trip costs ~100 s; the fix is to "
       "not take the trip.";

  EXPECT_GT(conn->effectiveRateLimit(), kFieldRateLimit / 2)
    << "Two minutes after a 4 s blip the cap must be usable again.";

  // The delivered fraction above cannot move until the cap has halved
  // TWICE (495000 offered against a 1500000 cap: one halving still
  // admits everything), so on its own it is blind to the first two
  // decreases. Count them directly. The blip spans ~4 s, inside one
  // refractory window, so it is one congestion episode and is entitled
  // to exactly one backoff.
  EXPECT_LE(decreases, 1)
    << "A single ~4 s handover blip applied " << decreases << " decreases. "
       "One congestion episode must cost at most one backoff — the "
       "2026-08-25 collapse was seven halvings from one such episode "
       "(#52).";
}

// THE OTHER SIDE OF THE GATE.
//
// Both tests above assert only that the cap does NOT fall. Nothing above
// asserts that it still falls when it genuinely should — and that gap is
// not hypothetical. The `review-plan` pass on this cycle's work plan
// modelled the originally-proposed EWMA smoothing of the received signal
// and found that at alpha = 0.2 the controller never registers congestion
// at all: the cap stays pinned at 1500000 for the whole recorded onset,
// and BOTH tests above go green on a controller that has stopped working.
// A tuning loop whose only oracle is a one-sided gate can converge there.
//
// So: a link whose real capacity drops and STAYS down. Not a recorded
// transient and not a handover blip — a sustained 10x reduction, the
// regime the admission controller exists for. The controller must find
// it, and must find it in seconds, because issue #52's over_horizon phase
// needs it capable of reacting again once the link returns.
//
// This test bounds the refractory backoff cap only WEAKLY. Its 40 s
// convergence deadline rules out a window grown to the minutes scale,
// but it was measured (#52 review round 1) to stay green with
// kAdmissionRefractoryMaximumMultiple set to 1.0 (no growth at all) AND
// to 1000 (a 5000 s ceiling), because the descent it measures is driven
// by the halvings rather than by the window. The constants themselves
// are pinned by AdmissionControl.RefractoryWindowGrowthIsBounded in
// test_admission_control.cpp; do not read this test as covering them.
TEST_F(AdmissionFieldReplay, SustainedRealLossMustConverge)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t0 = clock.now();
  auto t = t0;
  auto conn = make_connection();

  // Offered load is the field day's measured median; the path can carry
  // a tenth of it and no more, for the whole run.
  constexpr float kOfferedBps = 495000.0f;
  constexpr float kTrueCapacityBps = 150000.0f;
  constexpr int kSteps = 60;                    // ~118 s of feedback

  // Bounds, and why these numbers.
  //
  //  - CONVERGENCE DEADLINE 40 s: the controller must be at or below 2x
  //    true capacity by then. The design reaches it in ~18 s (halvings at
  //    t = 0, ~6, ~18 as the refractory window grows 5 -> 10 s), so 40 s
  //    is roughly two windows of slack — tight enough that a refractory
  //    cap grown to the minutes-scale fails here, loose enough not to
  //    pin the exact schedule.
  //  - BAND [0.5x, 2x] capacity in steady state: AIMD does not settle on
  //    a value, it oscillates — halving until the cap is under capacity,
  //    then recovering 10% per clean sample until it is over again. The
  //    band is what that oscillation is allowed to span. The lower half
  //    is the load-bearing half: a controller that collapses to the
  //    8192 floor on a link delivering 150 kB/s is the 2026-08-25 defect,
  //    and 0.5x is ~9x above that floor.
  constexpr double kConvergenceDeadlineSeconds = 40.0;
  const uint32_t kBandLow = static_cast<uint32_t>(kTrueCapacityBps * 0.5f);
  const uint32_t kBandHigh = static_cast<uint32_t>(kTrueCapacityBps * 2.0f);

  bool converged = false;
  double converged_at = -1.0;
  uint32_t lowest_after_convergence = kFieldRateLimit;
  uint32_t highest_after_convergence = 0;
  int steady_samples = 0;

  for(int i = 0; i < kSteps; ++i)
  {
    const float cap = static_cast<float>(conn->effectiveRateLimit());
    const float admitted = std::min(kOfferedBps, cap);
    send_over_interval(*conn, t, kFeedbackInterval, admitted);
    conn->update_last_receive_time(t.seconds(), 100, false);

    // The link is the binding constraint, not our gate: it carries what
    // it can carry and drops the rest.
    const float delivered = std::min(admitted, kTrueCapacityBps);
    conn->updateAdmissionControl(delivered, 0.0f, t);

    const double elapsed = (t - t0).seconds();
    const uint32_t now_cap = conn->effectiveRateLimit();
    if(!converged && now_cap <= kBandHigh)
    {
      converged = true;
      converged_at = elapsed;
    }
    // Judge the band only once the run has settled — the first 60 s
    // includes the descent itself, which legitimately sits above the
    // band on its way down.
    if(elapsed > 60.0)
    {
      ++steady_samples;
      lowest_after_convergence = std::min(lowest_after_convergence, now_cap);
      highest_after_convergence = std::max(highest_after_convergence, now_cap);
    }

    t = t + rclcpp::Duration::from_seconds(kFeedbackInterval);
  }

  ASSERT_TRUE(converged)
    << "The cap never came down to " << kBandHigh << " B/s on a link "
       "delivering " << kTrueCapacityBps << " B/s for two minutes. The "
       "controller has stopped detecting congestion — which is exactly "
       "how a smoothing/refractory tuning can make the two tests above "
       "pass while the control loop no longer works.";
  EXPECT_LT(converged_at, kConvergenceDeadlineSeconds)
    << "Converged, but after " << converged_at << " s. Sustained real "
       "loss has to be found in seconds: issue #52's over_horizon phase "
       "needs the controller able to react again inside a bounded window "
       "once the link returns, and a refractory window allowed to grow "
       "without bound defeats that.";

  ASSERT_GT(steady_samples, 0);
  EXPECT_GT(lowest_after_convergence, kBandLow)
    << "After converging, the cap dipped to " << lowest_after_convergence
    << " B/s against a real capacity of " << kTrueCapacityBps
    << ". Finding the capacity must not turn into collapsing past it — "
       "the field failure was a controller that kept going to the floor.";
  EXPECT_LE(highest_after_convergence, kBandHigh)
    << "After converging, the cap climbed back to "
    << highest_after_convergence << " B/s and stayed above the band. "
       "Additive recovery must not out-run the decrease on a link that "
       "is still degraded.";
}

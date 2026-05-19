// Unit tests for RemoteNode::getMissingPackets — the resend-loop
// algorithm tuned in issue #9. Drives the gap-scan / debounce /
// backoff / give-up paths against controlled times, using the
// test-only injection helpers exposed on RemoteNode so the suite
// doesn't have to bring up a sim-time-driven node.
//
// Reference field measurements (2026-04-21 BizzyBoat deployment):
//   - ~22% resend overhead on both WiFi and VPN
//   - 126% rx_duplicate on VPN (multiple copies of the same data)
//
// The four cases below cover the failure modes that produced those
// numbers; passing them does not prove the fix works in the field,
// but a regression in any of them would reproduce a piece of the
// 2026-04-21 shape.
//
// Time arithmetic uses RCL_ROS_TIME so the rclcpp::Time we construct
// matches the clock RemoteNode itself would observe under sim time.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/remote_node.h"
#include "udp_bridge/resend_constants.h"

namespace
{

constexpr rcl_clock_type_t kClock = RCL_ROS_TIME;

// Convenience: a rclcpp::Time built from a scalar offset.
rclcpp::Time t_at(double s)
{
  return rclcpp::Time(static_cast<int64_t>(s * 1e9), kClock);
}

// Owns a node so RemoteNode has the NodeInterfaces it needs at
// construction.
class ResendFixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
    node_ = std::make_shared<rclcpp::Node>("test_remote_node_resend");
    remote_ = std::make_unique<udp_bridge::RemoteNode>(
      "test-remote", "test-local",
      udp_bridge::RemoteNode::NodeInterfaces(*node_));
  }

  std::shared_ptr<rclcpp::Node> node_;
  std::unique_ptr<udp_bridge::RemoteNode> remote_;
};

}  // namespace

// Case 1: reorder-within-debounce — packets 1, 3 arrive within the
// debounce window; packet 2 arrives shortly after. Even though 2 is
// observed missing at the moment 3 arrives, the debounce hold gives
// it time to show up. No resend requested before 2 closes the gap.
TEST_F(ResendFixture, ReorderWithinDebounceProducesNoResend)
{
  // Packets arrive in 1, 3 order with timestamps 50 ms apart — well
  // inside the 100 ms debounce hold.
  remote_->recordReceivedPacketTimeForTest(1, t_at(1.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(1.050));

  // Scan at a moment shortly after packet 3 arrived. prev_received
  // (= packet 1 at t=1.000) is 60 ms in the past — under the debounce
  // hold — so packet 2 is not yet considered missing.
  auto rr = remote_->getMissingPackets(t_at(1.060));
  EXPECT_TRUE(rr.missing_packets.empty())
    << "Debounce should suppress request for K under reordered arrival";
}

// Case 2: debounce-then-request — packets 1, 3 arrive 150 ms apart,
// then no further arrivals. Once the debounce hold expires the gap
// at 2 is requested exactly once. A follow-up tick before the
// backoff window expires must NOT re-request.
TEST_F(ResendFixture, DebounceThenRequestEmitsOneResendPerCooldown)
{
  remote_->recordReceivedPacketTimeForTest(1, t_at(1.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(1.150));

  // First scan, comfortably past the debounce hold — expect packet 2
  // requested exactly once.
  auto rr1 = remote_->getMissingPackets(t_at(1.250));
  ASSERT_EQ(rr1.missing_packets.size(), 1u);
  EXPECT_EQ(rr1.missing_packets[0], 2u);

  // Second scan, just 50 ms later (still inside the kResendBackoffBase
  // = 200 ms cooldown). No re-request.
  auto rr2 = remote_->getMissingPackets(t_at(1.300));
  EXPECT_TRUE(rr2.missing_packets.empty())
    << "Re-request before cooldown expires would re-introduce the "
       "flat-cooldown amplification observed on 2026-04-21";
}

// Case 3: backoff-schedule — sustained miss across many ticks. The
// re-request intervals must follow min(base * 2^(attempts-1), cap):
// 0.2, 0.4, 0.8, 1.6, 1.6, 1.6, ... Each tick is the earliest moment
// at which the next attempt becomes legal; collected intervals must
// match the schedule until kSentPacketTTL kicks in.
TEST_F(ResendFixture, BackoffScheduleFollowsExponentialThenCap)
{
  remote_->recordReceivedPacketTimeForTest(1, t_at(0.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(0.200));

  std::vector<double> request_times;

  // Drive ticks at fine granularity and collect every moment the gap
  // is requested.
  for(double t = 0.300; t < 0.300 + udp_bridge::kSentPacketTTL.count(); t += 0.010)
  {
    auto rr = remote_->getMissingPackets(t_at(t));
    if(!rr.missing_packets.empty() && rr.missing_packets[0] == 2u)
      request_times.push_back(t);
  }

  // First request fires immediately at t = 0.300. Subsequent requests
  // come at intervals matching the backoff schedule.
  ASSERT_GE(request_times.size(), 5u)
    << "Should see at least 5 requests before TTL give-up in a 5 s window";

  std::vector<double> intervals;
  for(std::size_t i = 1; i < request_times.size(); ++i)
    intervals.push_back(request_times[i] - request_times[i - 1]);

  // Tolerance allows for the 10 ms tick discretization. Each interval
  // is at least the configured cooldown; the at-least direction is
  // what the algorithm guarantees.
  const double tick = 0.010;
  const double base = udp_bridge::kResendBackoffBase.count();
  const double cap = udp_bridge::kResendBackoffCap.count();
  EXPECT_GE(intervals[0], base - 1e-9)
    << "First inter-request interval should be >= base (" << base << "s)";
  EXPECT_LT(intervals[0], base + 2 * tick)
    << "First inter-request interval should be ~base, got " << intervals[0];
  EXPECT_GE(intervals[1], 2 * base - 1e-9);
  EXPECT_LT(intervals[1], 2 * base + 2 * tick);
  EXPECT_GE(intervals[2], 4 * base - 1e-9);
  EXPECT_LT(intervals[2], 4 * base + 2 * tick);
  // Cap kicks in at attempt 4.
  for(std::size_t i = 3; i < intervals.size(); ++i)
  {
    EXPECT_GE(intervals[i], cap - 1e-9)
      << "Interval " << i << " before cap kick-in";
    EXPECT_LT(intervals[i], cap + 2 * tick)
      << "Interval " << i << " above cap+tolerance";
  }
}

// Case 4: TTL give-up — sustained miss whose first-request time
// crosses kSentPacketTTL. The receiver must stop requesting it, bump
// resend_giveup_count_ by exactly one for that packet, and not issue
// any further requests.
TEST_F(ResendFixture, TtlGiveUpStopsRequestingAndIncrementsCounter)
{
  remote_->recordReceivedPacketTimeForTest(1, t_at(0.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(0.200));

  // Drive ticks well past kSentPacketTTL after the first-request
  // moment.
  uint32_t giveups_before = remote_->resendGiveupCount();
  const double first_request_time = 0.300;
  const double final_time = first_request_time + udp_bridge::kSentPacketTTL.count() + 1.0;

  bool ever_requested = false;
  for(double t = first_request_time; t < final_time; t += 0.050)
  {
    auto rr = remote_->getMissingPackets(t_at(t));
    if(!rr.missing_packets.empty() && rr.missing_packets[0] == 2u)
      ever_requested = true;
  }

  EXPECT_TRUE(ever_requested)
    << "Sanity: packet 2 should have been requested at least once";

  // Counter incremented exactly once for this packet.
  EXPECT_EQ(remote_->resendGiveupCount(), giveups_before + 1u);

  // Past give-up, the receiver must NOT keep requesting it even if
  // more ticks happen.
  for(double t = final_time; t < final_time + 1.0; t += 0.050)
  {
    auto rr = remote_->getMissingPackets(t_at(t));
    EXPECT_TRUE(rr.missing_packets.empty())
      << "Receiver should have stopped requesting packet 2 after "
         "TTL give-up, but saw a request at t=" << t;
  }
}

// Case 5 (regression for Must-Fix 1 from the 2026-05-18 sub-agent
// review): after a packet is given up on, the receiver must NOT
// re-request it just because subsequent arrivals keep the gap
// visible in the scan range. Without the given_up_packet_numbers_
// guard, the backoff loop's resend_state_[m] access would
// operator[]-default-construct a fresh entry the next tick, hit the
// "first observed" branch, and re-arm a full TTL window of attempts —
// bumping the give-up counter once per TTL cycle.
TEST_F(ResendFixture, GiveUpIsNotReArmedByOngoingArrivals)
{
  // Seed: packets 1, 3 around t=0 — gap at 2.
  remote_->recordReceivedPacketTimeForTest(1, t_at(0.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(0.200));

  // Drive past kSentPacketTTL to trigger give-up, AND inject a fresh
  // arrival at each tick so received_packet_times_ never empties out
  // (which would otherwise let clearReceivedPacketTimesBefore evict
  // the neighbors and remove gap 2 from the scan range entirely).
  // The new arrivals at higher packet numbers keep gap 2 visible:
  // begin()->first stays at 1 (or shifts upward) for as long as the
  // receive history retains a packet with number < 2.
  //
  // After the first give-up (one increment of resendGiveupCount()),
  // the counter must NOT keep climbing — the receiver must remember
  // it gave up on packet 2.
  // 2.5 TTL windows is enough that the bug, if present, would
  // produce 2-3 give-up cycles instead of one.
  const double start_t = 0.300;
  const double end_t = start_t + 2.5 * udp_bridge::kSentPacketTTL.count();
  uint64_t next_pn = 4;
  for(double t = start_t; t < end_t; t += 0.050)
  {
    // Inject a new neighbor at packet number above 3, so gap 2 stays
    // bounded on both sides (1 below, the new arrival above).
    remote_->recordReceivedPacketTimeForTest(next_pn, t_at(t));
    ++next_pn;
    remote_->getMissingPackets(t_at(t));
  }

  // Across 2.5 TTL windows we'd expect ~2-3 give-up cycles if the bug
  // were present. With the fix, the counter increments exactly once
  // for packet 2 and stays there.
  EXPECT_EQ(remote_->resendGiveupCount(), 1u)
    << "Give-up counter climbed past 1 — packet 2 was given up "
       "multiple times. The given_up_packet_numbers_ guard in "
       "getMissingPackets is missing or broken: the backoff loop's "
       "resend_state_[m] access is re-creating a fresh entry after "
       "the give-up sweep erased it, re-arming a full TTL window of "
       "attempts.";
}

// Case 6 (regression for C1 from the 2026-05-19 Copilot review): if
// a previously-requested resend ARRIVES, the give-up sweep must NOT
// later treat that packet as abandoned. Without the
// `resend_state_.erase(packet_number)` in unwrap()'s on-arrival path,
// the lingering resend_state_ entry sits there until its
// first_request_time crosses giveup_cutoff, at which point the
// give-up sweep would fire its WARN log and bump
// resend_giveup_count_ for a packet that actually arrived.
TEST_F(ResendFixture, RecoveredPacketDoesNotTriggerGiveUp)
{
  // Seed: packets 1, 3 around t=0 — gap at 2.
  remote_->recordReceivedPacketTimeForTest(1, t_at(0.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(0.200));

  // Drive a tick past the debounce hold so the receiver flags 2 as
  // missing and sets first_request_time[2] = ~0.300.
  auto rr = remote_->getMissingPackets(t_at(0.300));
  ASSERT_EQ(rr.missing_packets.size(), 1u);
  ASSERT_EQ(rr.missing_packets[0], 2u);

  // Packet 2 arrives at t=0.500 (resend succeeded, or originally
  // lost packet finally got delivered). This is the production
  // recovery path that the C1 fix protects.
  remote_->recordReceivedPacketTimeForTest(2, t_at(0.500));

  // Drive ticks past kSentPacketTTL. Without the C1 fix, the
  // give-up sweep at t > first_request_time + kSentPacketTTL would
  // process the lingering resend_state_[2] entry, bump the counter,
  // and emit a misleading WARN log. With the fix, unwrap() erased
  // resend_state_[2] when packet 2 arrived, so the sweep finds
  // nothing to give up on.
  uint32_t giveups_before = remote_->resendGiveupCount();
  const double start_t = 0.600;
  const double end_t = start_t + udp_bridge::kSentPacketTTL.count() + 1.0;
  for(double t = start_t; t < end_t; t += 0.100)
    remote_->getMissingPackets(t_at(t));

  EXPECT_EQ(remote_->resendGiveupCount(), giveups_before)
    << "Counter incremented for a packet that arrived. The "
       "resend_state_ entry for the arrived packet was not erased "
       "in unwrap()'s on-arrival branch — the give-up sweep is now "
       "firing for recovered packets, inflating "
       "Remote.resend_giveup_count in production.";
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

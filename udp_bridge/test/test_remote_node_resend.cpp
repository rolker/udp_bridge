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
    // Per-test node name suffix silences duplicate-node warnings from
    // the ROS graph briefly retaining the previous test case's name
    // after teardown (rclcpp::shutdown only runs once in main() after
    // the full suite).
    auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string node_name = std::string("test_remote_node_resend_") + info->name();
    node_ = std::make_shared<rclcpp::Node>(node_name);
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
//
// Test mechanics (fixed for R6 #3): to actually exercise the guard,
// gap 2 must remain in the gap-scan range [begin()->first+1,
// rbegin()->first) AFTER the give-up moment. Two pitfalls the naive
// setup falls into:
//
//   1. Anchor eviction: seed packets 1 + 3 at t=0/0.2 only, then drive
//      ticks. Once now > kSentPacketTTL, clearReceivedPacketTimesBefore
//      evicts both. begin() and rbegin() collapse, gap 2 falls out of
//      the scan range, and the test passes whether the guard exists
//      or not (the original R5 test).
//
//   2. Injection cascade: re-inject anchors AND inject a fresh packet
//      next_pn at each tick. The fresh packets age out one by one,
//      each becoming a new gap with its own give-up cycle. Counter
//      goes to ~50 across 2.5 TTL windows even with the guard intact
//      — the guard works for packet 2 but the other gaps drown it
//      out, masking what's being tested.
//
// The fix: stick to packets 1 and 3 only (gap at 2), and re-inject
// BOTH at a "just-inside-the-eviction-window" timestamp once t crosses
// kReceiveHistoryWindow. Packet 1 stays at begin(), packet 3 stays at
// rbegin(), gap-scan walks only i=2, no cascade of other gaps. The
// timestamp `now - kReceiveHistoryWindow + 1 ms` survives the strict-
// less-than eviction comparison and leaves prev_received_time (=
// packet 1's time) ~kReceiveHistoryWindow behind now — well below the
// 100 ms debounce hold, so gap 2 keeps flagging past give-up. The
// backoff loop then reaches `given_up_packet_numbers_.count(2)` and
// the test actually fails without the guard.
TEST_F(ResendFixture, GiveUpIsNotReArmedByOngoingArrivals)
{
  // Seed: packets 1, 3 around t=0 — gap at 2.
  remote_->recordReceivedPacketTimeForTest(1, t_at(0.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(0.200));

  // Drive past kSentPacketTTL to trigger give-up. Once t crosses
  // kReceiveHistoryWindow, re-inject BOTH anchor packets at a
  // just-inside-the-window timestamp so they survive eviction by
  // clearReceivedPacketTimesBefore while keeping the gap-scan anchor
  // (packet 1's time) well behind the debounce cutoff.
  //
  // After the first give-up (one increment of resendGiveupCount()),
  // the counter must NOT keep climbing — the receiver must remember
  // it gave up on packet 2.
  // 2.5 TTL windows is enough that the bug, if present, would
  // produce 2-3 give-up cycles instead of one.
  const double start_t = 0.300;
  const double window = udp_bridge::kReceiveHistoryWindow.count();
  const double end_t = start_t + 2.5 * udp_bridge::kSentPacketTTL.count();
  for(double t = start_t; t < end_t; t += 0.050)
  {
    if(t > window)
    {
      // Pick a timestamp just inside the eviction window. The 1 ms
      // buffer beats the strict-less-than eviction comparison; the
      // distance back from `t` (~window) is well past the debounce
      // hold (100 ms), so the gap stays flaggable.
      double safe_time = t - window + 0.001;
      remote_->recordReceivedPacketTimeForTest(1, t_at(safe_time));
      remote_->recordReceivedPacketTimeForTest(3, t_at(safe_time));
    }
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

// Case 7 (regression for the 2026-05-19 sub-agent review): remote
// restart must clear all three resend-tracking maps
// (received_packet_times_, resend_state_, given_up_packet_numbers_),
// not just the first. Before the give-up reorg this PR introduces,
// only received_packet_times_ existed; after the reorg, the restart
// path is responsible for nuking the other two maps as well. If a
// future change adds a fourth map and forgets to wire it into the
// restart clear, this test fails on the second-tick-no-bump assertion
// (lingering resend_state_ entries trigger fresh give-up cycles
// against the empty received_packet_times_ map).
//
// The resend_giveup_count_ counter is intentionally NOT reset on
// restart — header doc at remote_node.h:169-172 calls out that it's
// operator-facing and represents cumulative loss across the
// RemoteNode's lifetime. This test pins that contract too.
TEST_F(ResendFixture, RemoteRestartClearsAllResendState)
{
  // Set next_packet_number_ to a non-zero value so the second update()
  // triggers restart detection via `bridge_info.next_packet_number <
  // next_packet_number_`. Build a BridgeInfo with a single Remote
  // entry naming this node's local_name_ ("test-local") so the inner
  // restart-check loop in update() runs.
  udp_bridge::SourceInfo src;
  src.node_name = "test-remote";
  src.host = "127.0.0.1";
  src.port = 9999;

  udp_bridge_interfaces::msg::BridgeInfo bi_initial;
  bi_initial.next_packet_number = 100;
  udp_bridge_interfaces::msg::Remote remote_entry;
  remote_entry.name = "test-local";
  bi_initial.remotes.push_back(remote_entry);
  remote_->update(bi_initial, src);

  // Seed packets 1, 3 → gap at 2 → drive past TTL to populate
  // resend_state_, given_up_packet_numbers_, and bump
  // resend_giveup_count_ to 1.
  remote_->recordReceivedPacketTimeForTest(1, t_at(0.000));
  remote_->recordReceivedPacketTimeForTest(3, t_at(0.200));
  for(double t = 0.300; t < 0.300 + udp_bridge::kSentPacketTTL.count() + 0.1; t += 0.050)
    remote_->getMissingPackets(t_at(t));
  ASSERT_EQ(remote_->resendGiveupCount(), 1u)
    << "Sanity: setup should have populated give-up state before the restart";

  // Deliver a restart-signaling BridgeInfo (next_packet_number = 0
  // and 0 < 100 → triggers the restart-clear branch).
  udp_bridge_interfaces::msg::BridgeInfo bi_restart;
  bi_restart.next_packet_number = 0;
  bi_restart.remotes.push_back(remote_entry);
  remote_->update(bi_restart, src);

  // Follow-up tick well past the original TTL window. With the
  // restart-clear in place, resend_state_ is empty so the give-up
  // sweep has nothing to process — counter stays at 1, no throw, no
  // missing packets reported (received_packet_times_ is also empty so
  // the gap-scan has nothing to walk).
  for(double t = 10.0; t < 10.0 + udp_bridge::kSentPacketTTL.count() * 2; t += 0.100)
  {
    auto rr = remote_->getMissingPackets(t_at(t));
    EXPECT_TRUE(rr.missing_packets.empty())
      << "Gap scan should find nothing after restart cleared "
         "received_packet_times_; got " << rr.missing_packets.size()
      << " missing at t=" << t;
  }

  // The give-up counter is the load-bearing assertion: if a future
  // change drops `resend_state_.clear()` from the restart path,
  // lingering entry [2] has first_request_time < (now - kSentPacketTTL)
  // and the sweep above bumps the counter again — would observe 2+
  // here instead of 1.
  EXPECT_EQ(remote_->resendGiveupCount(), 1u)
    << "Counter climbed past 1 after restart. The restart path "
       "(remote_node.cpp:73-85) did not clear resend_state_, so the "
       "give-up sweep is re-firing for entries that survived the "
       "restart against an empty received_packet_times_ map.";
}

// Case 8: burst-loss debounce — pack 1 + pack 5 arrive 50 ms apart;
// packets 2, 3, 4 are lost in a single burst. The gap-scan walks
// [begin+1, last-1] with prev_received_time pinned to packet 1's
// receive time across the run of consecutive gaps. All three gaps
// share the same anchor (packet 1 at t=0), so they all flag together
// once the debounce hold expires — they do NOT each wait their own
// per-gap debounce. This pins the design intent commented at
// remote_node.cpp:308-312 against a future "optimization" that resets
// prev_received_time on gap entries.
TEST_F(ResendFixture, BurstLossAllGapsFlagTogetherAfterDebounce)
{
  remote_->recordReceivedPacketTimeForTest(1, t_at(0.000));
  remote_->recordReceivedPacketTimeForTest(5, t_at(0.050));

  // Tick at t=0.060: prev_received (= packet 1 at 0.000) is 60 ms old,
  // under the 100 ms debounce hold. No gaps flagged.
  auto rr_early = remote_->getMissingPackets(t_at(0.060));
  EXPECT_TRUE(rr_early.missing_packets.empty())
    << "Debounce should suppress all 3 gaps while prev_received is "
       "still inside the hold";

  // Tick at t=0.150: prev_received is now 150 ms old, past the hold.
  // All three gaps anchor on packet 1 and must flag in one tick.
  auto rr = remote_->getMissingPackets(t_at(0.150));
  ASSERT_EQ(rr.missing_packets.size(), 3u)
    << "Burst loss should flag all three gaps in one tick — they share "
       "an anchor (packet 1's receive time), not their own per-gap "
       "clocks. If a future change resets prev_received_time on gap "
       "entries, this test fails because each gap then waits its own "
       "100 ms hold and only the first one fires at t=0.150.";
  EXPECT_EQ(rr.missing_packets[0], 2u);
  EXPECT_EQ(rr.missing_packets[1], 3u);
  EXPECT_EQ(rr.missing_packets[2], 4u);
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

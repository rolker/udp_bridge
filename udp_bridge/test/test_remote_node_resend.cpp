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
// The eight cases below cover the failure modes that produced those
// numbers plus the regression cases that emerged during the review
// rounds for PR #13 (recovered-packet false-give-up, give-up re-arm
// via operator[] default-construct, remote-restart state clear,
// burst-loss debounce anchoring). Passing them does not prove the
// fix works in the field, but a regression in any of them would
// reproduce a piece of the 2026-04-21 shape or one of the failure
// modes the review surfaced.
//
// Time arithmetic uses RCL_ROS_TIME so the rclcpp::Time we construct
// matches the clock RemoteNode itself would observe under sim time.

#include <cstdint>
#include <cstdio>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "rclcpp/rclcpp.hpp"
#include "rcutils/logging.h"

#include "udp_bridge_interfaces/msg/resend_request.hpp"
#include "udp_bridge/connection.h"
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

// Issue #23: ResendRequests carry connection_id and dispatchResendRequest
// routes the response to that one connection only — replacing the
// previous broadcast across every active connection to the source remote,
// which amplified resend volume 2-4x and pushed bytes into rx-dead paths
// under the 2026-05-19 wifi-loss storm.
//
// Five tests below cover dispatch routing and the WARN-bookkeeping cap:
//   - DispatchRoutesToStampedConnectionOnly: the matched-id case — a
//     request stamped for "wifi" reaches only the wifi connection's
//     resend_packets, not vpn's.
//   - DispatchDropsWithoutBroadcastWhenStampedIdAbsent: receiver knows
//     a subset of the connection ids the sender knows about. Models
//     the transient post-restart / pre-BridgeInfo-reconciliation
//     window where the sender stamps for an id the receiver hasn't
//     (re-)registered yet.
//   - DispatchDropsWithoutBroadcastUnknownConnectionId: stamped id was
//     never registered on the receiver at all. Models a config-mismatch
//     coordinated-redeploy bug where the two bridges' connection-id
//     strings don't agree.
//   - DispatchDoesNotFuzzyMatchConnectionId: dispatch performs a
//     strict-equality lookup — a request stamped with the untruncated
//     form of an id already in the map must miss, not be silently
//     routed via a prefix match. The wire-contract truncation that
//     gives an in-band form to compare against lives in the private
//     receive plumbing of UDPBridge (resendMissingPackets / receive-side
//     clamp in decodeResendRequest); this test pins the dispatch
//     contract only, not that wire contract.
//   - DispatchMissWarnedIdsAreBounded: the bookkeeping table that
//     dedups first-miss WARN logs has a hard cap (kDispatchMissWarnedCap)
//     with FIFO eviction; a peer streaming distinct ids cannot grow
//     the table without bound.
// The lookup-miss cases share dispatch's miss branch (which emits
// WARN-on-first-miss-per-id, DEBUG thereafter — see
// dispatchResendRequest in remote_node.cpp). The behavioral invariant
// these tests assert is "no broadcast fallback to other connections",
// not "no log output". The test split documents the operational
// scenarios an operator might be chasing when that WARN/DEBUG log fires.

namespace
{

// Open a real UDP socket for the dispatch tests. Connection::send
// will hand this to sendto(); the loopback destination resolves to
// a real port whose receiver is no one, so the kernel cheerfully
// accepts the datagram. The assertion target is the per-connection
// counter, NOT the I/O — Connection::resend_packets increments the
// counter at function entry before any sendto call, so empty payloads
// or undeliverable destinations don't affect the test outcome. Caller
// closes via RAII.
class ScopedFd
{
public:
  ScopedFd() : fd_(::socket(AF_INET, SOCK_DGRAM, 0)) {}
  ~ScopedFd() { if(fd_ >= 0) ::close(fd_); }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  int get() const { return fd_; }
private:
  int fd_;
};

}  // namespace

// Routing case 1: a ResendRequest stamped for "wifi" reaches only the
// wifi connection's resend_packets — not vpn's.
TEST_F(ResendFixture, DispatchRoutesToStampedConnectionOnly)
{
  auto wifi = remote_->newConnection("wifi", "127.0.0.1", 9001);
  auto vpn  = remote_->newConnection("vpn",  "127.0.0.1", 9002);
  ASSERT_NE(wifi, nullptr);
  ASSERT_NE(vpn,  nullptr);

  udp_bridge_interfaces::msg::ResendRequest rr;
  rr.missing_packets = {1u};
  rr.connection_id = "wifi";

  ScopedFd sock;
  ASSERT_GE(sock.get(), 0);
  remote_->dispatchResendRequest(rr, sock.get(), t_at(2.0));

  EXPECT_EQ(wifi->resend_call_count_for_test(), 1u)
    << "Stamped-for connection should receive exactly one resend dispatch.";
  EXPECT_EQ(vpn->resend_call_count_for_test(), 0u)
    << "Non-stamped connection must not receive a resend — this is the "
       "amplification bug issue #23 is fixing.";
}

// Routing case 2: stamped id is absent from the receiver's connections_
// (subset state). Models the race window where the receiver hasn't
// re-registered the connection yet but the sender already stamped for
// it. Dispatch must no-op (no broadcast fallback to other live
// connections — that's the old behavior this issue removes). Dispatch
// also emits a WARN log on the first such miss per id; this test
// asserts the routing invariant, not the log output.
TEST_F(ResendFixture, DispatchDropsWithoutBroadcastWhenStampedIdAbsent)
{
  // Receiver knows only "vpn" right now; sender stamped for "wifi".
  auto vpn = remote_->newConnection("vpn", "127.0.0.1", 9002);
  ASSERT_NE(vpn, nullptr);

  udp_bridge_interfaces::msg::ResendRequest rr;
  rr.missing_packets = {1u};
  rr.connection_id = "wifi";

  ScopedFd sock;
  ASSERT_GE(sock.get(), 0);
  remote_->dispatchResendRequest(rr, sock.get(), t_at(2.0));

  EXPECT_EQ(vpn->resend_call_count_for_test(), 0u)
    << "Lookup miss must not fan out to the other live connection — "
       "that would reintroduce the broadcast behavior issue #23 removes.";
}

// Routing case 3: stamped id was never registered (config mismatch
// across coordinated redeploys). Same no-broadcast-fallback invariant
// as case 2; differs in known_ids log content (which is operator-facing,
// not asserted here).
TEST_F(ResendFixture, DispatchDropsWithoutBroadcastUnknownConnectionId)
{
  auto wifi = remote_->newConnection("wifi", "127.0.0.1", 9001);
  auto vpn  = remote_->newConnection("vpn",  "127.0.0.1", 9002);
  ASSERT_NE(wifi, nullptr);
  ASSERT_NE(vpn,  nullptr);

  udp_bridge_interfaces::msg::ResendRequest rr;
  rr.missing_packets = {1u};
  rr.connection_id = "iridium";  // never registered on this RemoteNode

  ScopedFd sock;
  ASSERT_GE(sock.get(), 0);
  remote_->dispatchResendRequest(rr, sock.get(), t_at(2.0));

  EXPECT_EQ(wifi->resend_call_count_for_test(), 0u);
  EXPECT_EQ(vpn->resend_call_count_for_test(),  0u);
}

// Routing case 4: dispatch is strict-match — it does not fuzzy-match
// or auto-truncate. The receiver's connections_ map is keyed on the
// truncated id (the on-wire SequencedPacketHeader is a fixed
// char[maximum_connection_id_size]; WrappedPacket truncates to 7
// chars + null at wrapped_packet.cpp:31-32, and connections_ is
// populated from packet headers). Dispatch looks up
// connections_[rr.connection_id] exactly — no prefix match, no
// length normalization.
//
// Scope note: this test verifies the dispatch-side strict-match
// invariant only. The full wire contract — "any id longer than 7
// chars routes correctly in production" — is enforced by the
// combination of (a) sender-side truncation in
// UDPBridge::resendMissingPackets and (b) receive-side clamp in
// UDPBridge::decodeResendRequest, both of which apply BEFORE
// dispatchResendRequest is called. Neither has a dedicated unit
// test because both are inside the private wire-decode plumbing of
// UDPBridge; their integration is verified by the package's
// end-to-end resend behavior in the field rather than at this seam.
// A regression that re-introduces the pre-fix bug (sender stamps
// the full untruncated id) would still route correctly today
// because the receive-side clamp would normalize the id before
// reaching dispatch. So a strict regression test for the wire
// contract would require either a friend-declared test entry to
// decodeResendRequest or a wire-format integration test that
// constructs and dispatches a real packet — both out of scope for
// this unit suite.
TEST_F(ResendFixture, DispatchDoesNotFuzzyMatchConnectionId)
{
  // The receiver's connection key is the truncated form (what would
  // arrive in the wire packet header). "starlink" (8 chars) becomes
  // "starlin" (7 chars + implicit null in the char array).
  auto truncated = remote_->newConnection("starlin", "127.0.0.1", 9003);
  ASSERT_NE(truncated, nullptr);

  ScopedFd sock;
  ASSERT_GE(sock.get(), 0);

  {
    // Exact-match: rr stamped with the truncated key → routes.
    udp_bridge_interfaces::msg::ResendRequest rr;
    rr.missing_packets = {1u};
    rr.connection_id = "starlin";
    remote_->dispatchResendRequest(rr, sock.get(), t_at(2.0));
    EXPECT_EQ(truncated->resend_call_count_for_test(), 1u)
      << "Exact-key stamp must match the connection's key.";
  }
  {
    // No fuzzy match: rr stamped with the untruncated form — even
    // though "starlink" begins with "starlin", dispatch must do a
    // strict equality lookup. If a future refactor introduces
    // prefix-match behavior at the dispatch site, this counter
    // bumps and the test fails — surfacing the change before it
    // can silently mis-route a forged id.
    udp_bridge_interfaces::msg::ResendRequest rr;
    rr.missing_packets = {1u};
    rr.connection_id = "starlink";
    remote_->dispatchResendRequest(rr, sock.get(), t_at(2.0));
    EXPECT_EQ(truncated->resend_call_count_for_test(), 1u)
      << "Dispatch lookup must be strict-equality. A counter bump "
         "here would mean dispatch is fuzzy-matching ids, which "
         "could mis-route a forged or stale id to a real connection.";
  }
}

// Routing case 5: bookkeeping cap. dispatchResendRequest tracks
// already-WARNed ids in a bounded FIFO to keep memory pressure
// proportional to the configured connection-id space, not to whatever
// the peer sends (a peer that streamed many distinct ids could
// otherwise grow the table without bound — see the field comment on
// dispatch_miss_warned_ids_ in remote_node.h). This test pumps more
// distinct ids than the cap and asserts the table size stops growing.
// The "ids each cause one dispatch call" wiring is irrelevant here —
// no connection is registered, so every dispatch is a lookup miss,
// which is exactly the path we're stressing.
TEST_F(ResendFixture, DispatchMissWarnedIdsAreBounded)
{
  // The implementation cap is RemoteNode::kDispatchMissWarnedCap (256
  // at the time of writing). We don't import the constant directly —
  // the assertion is "size stays <= the cap and converges to the cap
  // when we push past it", expressed in terms of the count returned
  // by dispatchMissWarnedIdCountForTest(). The specific cap value is
  // an implementation choice, not a wire contract.
  ScopedFd sock;
  ASSERT_GE(sock.get(), 0);

  // Every distinct id in this loop fires a first-miss WARN. To keep
  // the captured test log readable (and CI fast) we silence the
  // node's logger for the duration of the push, then restore. The
  // cap-enforcement assertion uses dispatchMissWarnedIdCountForTest,
  // not log inspection, so silencing has no effect on what the test
  // proves.
  auto logger = node_->get_logger();
  auto prev_level = rcutils_logging_get_logger_level(logger.get_name());
  logger.set_level(rclcpp::Logger::Level::Error);

  // 32 above any plausible cap — the test was previously pushing
  // 1024, which emitted ~1024 WARN lines per run. The cap is 256 at
  // the time of writing; pushing 288 still proves eviction fires
  // with comfortable margin (32 distinct evictions exercised).
  const std::size_t kPushCount = 288;
  for(std::size_t i = 0; i < kPushCount; ++i)
  {
    udp_bridge_interfaces::msg::ResendRequest rr;
    rr.missing_packets = {1u};
    // 7-char ids (the post-clamp wire size). Make each one unique by
    // appending a small alpha suffix derived from i. The actual
    // string values don't matter — only that they're distinct.
    char id[8];
    std::snprintf(id, sizeof(id), "m%05zu", i % 100000);
    rr.connection_id = id;
    remote_->dispatchResendRequest(rr, sock.get(), t_at(2.0 + static_cast<double>(i) * 0.001));
  }

  // Restore the prior logger level so subsequent tests behave
  // normally. RCUTILS_LOG_SEVERITY_* and rclcpp::Logger::Level use
  // the same integer values (0/10/20/30/40/50 for unset/debug/info/
  // warn/error/fatal), so the cast is well-defined.
  logger.set_level(static_cast<rclcpp::Logger::Level>(prev_level));

  // After pushing kPushCount distinct ids, the bookkeeping must
  // hold exactly the cap (we pushed kPushCount > cap, so it must
  // saturate to the cap value). Importing the constant directly
  // here is a small coupling to implementation, but it lets the
  // test catch a "doubled cap" regression that the previous loose
  // ceiling (<= 512u) would have permitted.
  std::size_t count = remote_->dispatchMissWarnedIdCountForTest();
  EXPECT_EQ(count, udp_bridge::RemoteNode::dispatchMissWarnedCapForTest())
    << "Bookkeeping size does not match the configured cap. Either "
       "the cap regressed (likely doubled), or eviction failed to "
       "run at the right boundary.";

  // FIFO order check: of the kPushCount ids inserted in sequence,
  // the most-recently-pushed should still be in the table (it was
  // just inserted at the tail), and the earliest pushed ids should
  // have been evicted (they fell off the head when the table
  // saturated). A regression that evicted from the tail instead of
  // the head — or evicted arbitrarily — would still satisfy the
  // size assertion above but would fail these.
  char head_id[8];   // earliest inserted (should be evicted)
  char tail_id[8];   // most recent (should be retained)
  std::snprintf(head_id, sizeof(head_id), "m%05zu", static_cast<std::size_t>(0));
  std::snprintf(tail_id, sizeof(tail_id), "m%05zu", kPushCount - 1);
  EXPECT_FALSE(remote_->isDispatchMissWarnedForTest(head_id))
    << "Earliest-inserted id (" << head_id << ") is still in the "
       "bookkeeping after pushing kPushCount > cap entries. FIFO "
       "eviction is broken — likely evicting from the tail or "
       "evicting arbitrarily.";
  EXPECT_TRUE(remote_->isDispatchMissWarnedForTest(tail_id))
    << "Most-recently-inserted id (" << tail_id << ") is missing "
       "from the bookkeeping. Insert is broken — or eviction "
       "happened immediately after insert.";
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

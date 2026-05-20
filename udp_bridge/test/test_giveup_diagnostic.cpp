// Tests for the resend-give-up rate / threshold computation (#22).
//
// Two layers of tests live here:
//
// 1. `computeGiveupDiagnostic` (pure free function) — exhaustive
//    arithmetic and threshold-boundary coverage. No state, no clock.
//
// 2. `stepGiveupDiagnostic` (stateful helper used by
//    UDPBridge::diagnoseRemoteGiveups) — wrapper-side behavior:
//    first-call initialization, sequential state-update ordering,
//    counter reset across calls, multi-state independence. These
//    tests cover the map-update-ordering invariant that the
//    UDPBridge wrapper depends on without needing to bring up a
//    real UDPBridge node (which would require sockets / executors).
//
// Mutex discipline of the UDPBridge wrapper is not directly testable
// here (no thread safety harness); the helper's pure-by-design
// semantics mean the only way the wrapper can go wrong is by failing
// to hold the lock — caught by code review, not unit test.

#include "udp_bridge/giveup_diagnostic.h"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "diagnostic_msgs/msg/diagnostic_status.hpp"
#include "rclcpp/time.hpp"

namespace
{

using udp_bridge::computeGiveupDiagnostic;
using udp_bridge::GiveupDiagnostic;
using udp_bridge::GiveupRateState;
using udp_bridge::stepGiveupDiagnostic;
using udp_bridge::validateGiveupThresholds;
using DS = diagnostic_msgs::msg::DiagnosticStatus;

constexpr double kWarn = 5.0;
constexpr double kError = 50.0;
constexpr rcl_clock_type_t kClock = RCL_ROS_TIME;

rclcpp::Time t_at(double seconds)
{
  return rclcpp::Time(static_cast<int64_t>(seconds * 1e9), kClock);
}

}  // namespace

TEST(GiveupDiagnostic, SteadyStateRateBelowWarnIsOk)
{
  // 4 give-ups over 1 second = 4/s, below the 5/s warn threshold.
  GiveupDiagnostic d = computeGiveupDiagnostic(100, 104, 1.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 4.0);
  EXPECT_EQ(d.total, 104u);
}

TEST(GiveupDiagnostic, RateAtWarnThresholdFiresWarn)
{
  // Exactly at the threshold — inclusive comparison fires WARN.
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 5, 1.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 5.0);
}

TEST(GiveupDiagnostic, RateAboveWarnBelowErrorFiresWarn)
{
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 30, 1.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 30.0);
}

TEST(GiveupDiagnostic, RateAtErrorThresholdFiresError)
{
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 50, 1.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::ERROR);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 50.0);
}

TEST(GiveupDiagnostic, RateFarAboveErrorFiresError)
{
  // 2026-05-19 deployment hit ~700/s burst.
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 700, 1.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::ERROR);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 700.0);
}

TEST(GiveupDiagnostic, FirstCallNoPriorState)
{
  // prev_count=0 paired with elapsed_s=0 represents the first call:
  // there's no baseline so we cannot compute a rate yet. Must NOT
  // divide; must return rate=0/OK and surface the current cumulative.
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 42, 0.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 42u);
}

TEST(GiveupDiagnostic, ZeroElapsedReturnsZeroRate)
{
  // Two ticks within the same sim-time `now()` (or any zero-elapsed
  // window) must not divide by zero — return rate=0/OK regardless of
  // the counter delta.
  GiveupDiagnostic d = computeGiveupDiagnostic(100, 200, 0.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 200u);
}

TEST(GiveupDiagnostic, NegativeElapsedReturnsZeroRate)
{
  // Clock-jump backward (NTP correction, sim-time reset) must collapse
  // to "no measurable rate" rather than emit a garbage negative rate.
  GiveupDiagnostic d = computeGiveupDiagnostic(100, 105, -0.5, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
}

TEST(GiveupDiagnostic, CounterResetReturnsZeroRate)
{
  // Per #21, the give-up counter has been observed to rewind across a
  // sender restart. `current < prev` must NOT compute as a negative
  // rate (would underflow the uint32 subtract and emit a wildly large
  // rate). Reset path: rate=0, level=OK, total reflects current.
  GiveupDiagnostic d = computeGiveupDiagnostic(1'000'000, 5, 1.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 5u);
}

TEST(GiveupDiagnostic, NoActivityReturnsOk)
{
  // current == prev means no give-up events happened this window.
  // Must short-circuit to OK regardless of threshold values — the
  // diagnostic reports activity, not its absence.
  GiveupDiagnostic d = computeGiveupDiagnostic(100, 100, 1.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 100u);
}

TEST(GiveupDiagnostic, ZeroDeltaWithZeroThresholdsStaysOk)
{
  // Regression for Copilot round-3 on PR #24: validateGiveupThresholds
  // accepts 0/0, and without the no-activity short-circuit the `>=`
  // comparison fires ERROR on every quiet tick (0.0 >= 0.0 is true).
  // The no-activity branch must run before threshold comparison so a
  // 0/0 pair behaves as "WARN/ERROR on any give-up activity, OK
  // otherwise" rather than "permanently non-OK".
  GiveupDiagnostic d = computeGiveupDiagnostic(
      /*prev=*/5, /*current=*/5, /*elapsed=*/1.0,
      /*warn=*/0.0, /*error=*/0.0);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 5u);
}

TEST(GiveupDiagnostic, ZeroWarnFiresOnAnyActivity)
{
  // Companion to ZeroDeltaWithZeroThresholdsStaysOk: validates that
  // warn=0 still means "WARN on any non-zero rate", not "WARN never".
  // With current > prev and elapsed > 0, the threshold branch runs and
  // a rate of 1/s is >= warn=0 → WARN (since 1 < error=10 → not ERROR).
  GiveupDiagnostic d = computeGiveupDiagnostic(
      /*prev=*/0, /*current=*/1, /*elapsed=*/1.0,
      /*warn=*/0.0, /*error=*/10.0);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 1.0);
}

TEST(GiveupDiagnostic, LongWindowAveragesCorrectly)
{
  // 60 give-ups across a 60-second window = 1/s steady, below WARN.
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 60, 60.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 1.0);
}

TEST(GiveupDiagnostic, JustBelowWarnStaysOk)
{
  // 4.99/s — strictly less than the threshold, must NOT cross.
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 499, 100.0, kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 4.99);
}

TEST(GiveupDiagnostic, CustomThresholdsRespected)
{
  // Tighter thresholds (e.g., a deployment that lowers WARN to 1/s
  // per the deferred Open Question 2). Verify the function honors
  // the caller's thresholds, not the calibrated defaults.
  GiveupDiagnostic d = computeGiveupDiagnostic(0, 2, 1.0, /*warn=*/1.0, /*error=*/10.0);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 2.0);
}

// ----- stepGiveupDiagnostic (wrapper-side state) -----

// First call: state starts with has_previous=false. Must NOT compute
// a rate from the default-constructed last_publish_time (which is
// epoch=0 — the difference against a real `now` would be huge). Rate
// must be 0/OK regardless of current_count. State is then populated
// so the second call has a baseline.
TEST(GiveupRateState, FirstCallReportsZeroRateAndPopulatesState)
{
  GiveupRateState state;
  EXPECT_FALSE(state.has_previous);

  GiveupDiagnostic d = stepGiveupDiagnostic(state, 42, t_at(100.0), kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 42u);

  EXPECT_TRUE(state.has_previous);
  EXPECT_EQ(state.last_count, 42u);
  EXPECT_DOUBLE_EQ(state.last_publish_time.seconds(), 100.0);
}

// Two-call sequence: second call sees the first's count + time as
// `prev`. This pins the map-update-ordering invariant — if
// stepGiveupDiagnostic wrote the new sample before reading the
// previous, this test would fail with rate=0 instead of rate=4.
TEST(GiveupRateState, SecondCallComputesRateFromFirstCallBaseline)
{
  GiveupRateState state;
  stepGiveupDiagnostic(state, 100, t_at(10.0), kWarn, kError);

  GiveupDiagnostic d = stepGiveupDiagnostic(state, 104, t_at(11.0), kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 4.0);
  EXPECT_EQ(d.total, 104u);

  EXPECT_EQ(state.last_count, 104u);
  EXPECT_DOUBLE_EQ(state.last_publish_time.seconds(), 11.0);
}

// Counter reset across calls (sender restart per #21). The second
// call sees current < prev. Wrapper must NOT underflow the subtract;
// computeGiveupDiagnostic's reset branch returns rate=0/OK, and the
// state must update to the new (smaller) count so the third call
// continues from the new baseline.
TEST(GiveupRateState, CounterResetAcrossCallsHandledByWrapper)
{
  GiveupRateState state;
  stepGiveupDiagnostic(state, 1'000'000, t_at(10.0), kWarn, kError);

  GiveupDiagnostic d = stepGiveupDiagnostic(state, 5, t_at(11.0), kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 5u);
  EXPECT_EQ(state.last_count, 5u);

  // Third call, post-reset: rate computed from the new baseline.
  GiveupDiagnostic d2 = stepGiveupDiagnostic(state, 8, t_at(12.0), kWarn, kError);
  EXPECT_EQ(d2.level, DS::OK);
  EXPECT_DOUBLE_EQ(d2.rate_per_s, 3.0);
}

// Same-remote queried twice within the same `now()` tick: elapsed=0,
// rate=0/OK regardless of count delta. State updates so subsequent
// calls with real elapsed see the latest count as prev.
TEST(GiveupRateState, ZeroElapsedBetweenStepsReturnsZeroRate)
{
  GiveupRateState state;
  stepGiveupDiagnostic(state, 100, t_at(10.0), kWarn, kError);

  GiveupDiagnostic d = stepGiveupDiagnostic(state, 200, t_at(10.0), kWarn, kError);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_DOUBLE_EQ(d.rate_per_s, 0.0);
  EXPECT_EQ(d.total, 200u);

  // Third call moves time forward; rate computed from the second-call
  // count (state was correctly updated even when rate was suppressed).
  GiveupDiagnostic d2 = stepGiveupDiagnostic(state, 210, t_at(11.0), kWarn, kError);
  EXPECT_EQ(d2.level, DS::WARN);
  EXPECT_DOUBLE_EQ(d2.rate_per_s, 10.0);
}

// Two independent states track separately. This is the wrapper's
// multi-remote independence story: each remote has its own
// GiveupRateState in the giveup_rate_state_ map, so stepping one does
// not perturb the other. (UDPBridge::diagnoseRemoteGiveups indexes
// the map by remote_name to get the per-remote state; this test
// exercises the underlying separation invariant.)
TEST(GiveupRateState, TwoIndependentStatesDoNotInterfere)
{
  GiveupRateState alpha;
  GiveupRateState beta;

  stepGiveupDiagnostic(alpha, 100, t_at(10.0), kWarn, kError);
  stepGiveupDiagnostic(beta, 500, t_at(10.0), kWarn, kError);

  GiveupDiagnostic da = stepGiveupDiagnostic(alpha, 104, t_at(11.0), kWarn, kError);
  GiveupDiagnostic db = stepGiveupDiagnostic(beta, 1000, t_at(11.0), kWarn, kError);

  EXPECT_DOUBLE_EQ(da.rate_per_s, 4.0);    // alpha rate over its own window
  EXPECT_DOUBLE_EQ(db.rate_per_s, 500.0);  // beta rate, far above ERROR
  EXPECT_EQ(da.level, DS::OK);
  EXPECT_EQ(db.level, DS::ERROR);
}

// Wrapper-side threshold change: if the caller passes new thresholds
// on a subsequent step (simulating an OnSetParametersCallback live
// update), the new thresholds take effect immediately — the state
// struct holds no copy of the thresholds.
TEST(GiveupRateState, ThresholdChangeTakesEffectOnNextStep)
{
  GiveupRateState state;
  stepGiveupDiagnostic(state, 100, t_at(10.0), kWarn, kError);

  // First call at the original 5/50 thresholds: rate=4 -> OK.
  GiveupDiagnostic d1 = stepGiveupDiagnostic(state, 104, t_at(11.0), kWarn, kError);
  EXPECT_EQ(d1.level, DS::OK);

  // Operator tightens WARN to 1/s mid-session; next call fires WARN
  // immediately at the same rate=4.
  GiveupDiagnostic d2 = stepGiveupDiagnostic(state, 108, t_at(12.0), /*warn=*/1.0, kError);
  EXPECT_EQ(d2.level, DS::WARN);
  EXPECT_DOUBLE_EQ(d2.rate_per_s, 4.0);
}

// ----- validateGiveupThresholds -----

TEST(GiveupThresholdValidation, DefaultPairIsAccepted)
{
  // The calibrated 5/50 defaults must validate cleanly — otherwise the
  // on_configure path would fail every launch.
  auto v = validateGiveupThresholds(5.0, 50.0);
  EXPECT_TRUE(v.ok);
  EXPECT_TRUE(v.reason.empty());
}

TEST(GiveupThresholdValidation, EqualWarnAndErrorIsAccepted)
{
  // warn == error collapses the WARN band to a single point, but it is
  // not invalid; an operator may want only OK/ERROR transitions.
  auto v = validateGiveupThresholds(10.0, 10.0);
  EXPECT_TRUE(v.ok);
}

TEST(GiveupThresholdValidation, ZeroThresholdsAreAccepted)
{
  // 0/0 is a pathological-but-valid configuration: it means every
  // give-up event fires ERROR (rate > 0 trips the `>= 0` comparison).
  // The validator's job is range/relation checks, not intent guessing.
  // Note: the diagnostic computation has a no-activity short-circuit
  // (computeGiveupDiagnostic returns OK when current == prev) so a
  // 0/0 pair doesn't make every quiet tick fire — see
  // ZeroDeltaWithZeroThresholdsStaysOk below for the regression.
  auto v = validateGiveupThresholds(0.0, 0.0);
  EXPECT_TRUE(v.ok);
}

TEST(GiveupThresholdValidation, NegativeWarnIsRejected)
{
  auto v = validateGiveupThresholds(-1.0, 50.0);
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.reason.find("resend_giveup_warn_rate_per_s"), std::string::npos);
  EXPECT_NE(v.reason.find(">= 0"), std::string::npos);
}

TEST(GiveupThresholdValidation, NegativeErrorIsRejected)
{
  auto v = validateGiveupThresholds(5.0, -10.0);
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.reason.find("resend_giveup_error_rate_per_s"), std::string::npos);
  EXPECT_NE(v.reason.find(">= 0"), std::string::npos);
}

TEST(GiveupThresholdValidation, NanWarnIsRejected)
{
  // NaN comparisons return false, so a NaN threshold silently disables
  // the diagnostic. Must be rejected with an operator-actionable reason.
  auto v = validateGiveupThresholds(std::numeric_limits<double>::quiet_NaN(), 50.0);
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.reason.find("resend_giveup_warn_rate_per_s"), std::string::npos);
  EXPECT_NE(v.reason.find("finite"), std::string::npos);
}

TEST(GiveupThresholdValidation, NanErrorIsRejected)
{
  auto v = validateGiveupThresholds(5.0, std::numeric_limits<double>::quiet_NaN());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.reason.find("resend_giveup_error_rate_per_s"), std::string::npos);
  EXPECT_NE(v.reason.find("finite"), std::string::npos);
}

TEST(GiveupThresholdValidation, PositiveInfinityIsRejected)
{
  // Infinity is finite-via-comparison but isfinite() rejects it. An
  // infinite threshold could never be exceeded, so the diagnostic would
  // never fire — same failure mode as NaN, different IEEE bit pattern.
  auto v = validateGiveupThresholds(5.0, std::numeric_limits<double>::infinity());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.reason.find("finite"), std::string::npos);
}

TEST(GiveupThresholdValidation, InvertedPairIsRejected)
{
  // warn > error would make the WARN band empty: rates above warn would
  // also be above error and fire ERROR, never WARN. Operator-error
  // signal — caught with both values printed so the operator can fix it.
  auto v = validateGiveupThresholds(/*warn=*/50.0, /*error=*/5.0);
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.reason.find("resend_giveup_warn_rate_per_s"), std::string::npos);
  EXPECT_NE(v.reason.find("resend_giveup_error_rate_per_s"), std::string::npos);
  EXPECT_NE(v.reason.find("<="), std::string::npos);
}

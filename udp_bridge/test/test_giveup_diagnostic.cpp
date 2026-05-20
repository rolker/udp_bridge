// Tests for the resend-give-up rate / threshold computation (#22).
//
// These tests exercise the free function `computeGiveupDiagnostic`
// directly — no UDPBridge instance, no rclcpp executor, no clock
// fixture. The time-dependence is moved out of the function into the
// caller (elapsed_s is a parameter), so tests are deterministic and
// instantaneous.
//
// Wrapper-side behavior (per-remote map state, lock discipline,
// integration with diagnostic_updater) is intentionally NOT covered
// here — see issue #22 plan §4.

#include "udp_bridge/giveup_diagnostic.h"

#include <gtest/gtest.h>

#include "diagnostic_msgs/msg/diagnostic_status.hpp"

namespace
{

using udp_bridge::computeGiveupDiagnostic;
using udp_bridge::GiveupDiagnostic;
using DS = diagnostic_msgs::msg::DiagnosticStatus;

constexpr double kWarn = 5.0;
constexpr double kError = 50.0;

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

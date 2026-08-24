// The "dropped since the last tick" baseline (issue #51).
//
// The publish- and relay-queue diagnostics report the increase in a
// cumulative drop counter since the previous tick, so a single historical
// drop does not latch WARN forever. The baseline that makes that possible
// has two writers: the diagnostic timer, and on_deactivate, which
// re-baselines so the backlog the operator just discarded is not reported
// as link loss. The lifecycle transition callback does not run in
// periodic_group_, so the two interleave.
//
// Held in a plain uint64_t and assigned by both, that interleaving both
// raced and gave a wrong answer -- the tick reads total 100, on_deactivate
// stores 120, the tick computes 100 - 120 and underflows to ~2^64, so the
// discarded backlog is published as an enormous burst of link loss. These
// tests pin the arithmetic that replaced it.

#include "udp_bridge/drop_baseline.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udp_bridge::advanceDropBaseline;
using udp_bridge::dropsSinceBaseline;

// The ordinary tick: the delta is the increase, and the baseline moves up.
TEST(DropBaseline, ReportsTheIncreaseSinceTheLastTick)
{
  std::atomic<uint64_t> baseline{0};

  EXPECT_EQ(dropsSinceBaseline(0, advanceDropBaseline(baseline, 0)), 0u);
  EXPECT_EQ(dropsSinceBaseline(7, advanceDropBaseline(baseline, 7)), 7u);
  EXPECT_EQ(baseline.load(), 7u);
  // Nothing new since: no drops to report, no latched WARN.
  EXPECT_EQ(dropsSinceBaseline(7, advanceDropBaseline(baseline, 7)), 0u);
  EXPECT_EQ(dropsSinceBaseline(10, advanceDropBaseline(baseline, 10)), 3u);
}

// The regression. A tick that read its total BEFORE a deactivate
// re-baselined must not report a negative delta as ~2^64 drops.
TEST(DropBaseline, StaleTotalAgainstANewerBaselineDoesNotUnderflow)
{
  std::atomic<uint64_t> baseline{0};

  // The tick reads the total...
  const uint64_t total_read_by_the_tick = 100;
  // ...then on_deactivate folds the discarded backlog in and re-baselines.
  advanceDropBaseline(baseline, 120);
  // ...and only now does the tick get to compute its delta.
  const uint64_t recent = dropsSinceBaseline(
    total_read_by_the_tick,
    advanceDropBaseline(baseline, total_read_by_the_tick));

  EXPECT_EQ(recent, 0u)
    << "an operator-caused backlog must not be reported as link loss";
  EXPECT_NE(recent, std::numeric_limits<uint64_t>::max() - 19)
    << "this is the 100 - 120 underflow the plain uint64_t produced";
}

// A stale total must not drag the baseline backwards either: doing so would
// re-report the same drops on the following tick.
TEST(DropBaseline, BaselineNeverMovesBackwards)
{
  std::atomic<uint64_t> baseline{0};

  advanceDropBaseline(baseline, 120);
  advanceDropBaseline(baseline, 100);
  EXPECT_EQ(baseline.load(), 120u);

  // So the next tick, at the same cumulative total, reports nothing.
  EXPECT_EQ(dropsSinceBaseline(120, advanceDropBaseline(baseline, 120)), 0u);
}

// Concurrent advances (the tick and the transition callback) must partition
// the increase: every drop is reported exactly once across all callers, and
// none is reported twice.
TEST(DropBaseline, ConcurrentAdvancesReportEachDropExactlyOnce)
{
  constexpr uint64_t kTotal = 20000;
  constexpr int kThreads = 4;

  std::atomic<uint64_t> baseline{0};
  std::atomic<uint64_t> reported{0};
  std::vector<std::thread> threads;

  for(int t = 0; t < kThreads; ++t)
    threads.emplace_back([&]
    {
      for(uint64_t total = 1; total <= kTotal; ++total)
        reported.fetch_add(
          dropsSinceBaseline(total, advanceDropBaseline(baseline, total)));
    });
  for(auto& thread: threads)
    thread.join();

  EXPECT_EQ(baseline.load(), kTotal);
  EXPECT_EQ(reported.load(), kTotal)
    << "the sum of every reported delta must equal the total exactly -- "
       "more means a drop was reported twice, less means one was lost";
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

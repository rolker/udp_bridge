// Per-connection diagnostic level + summary (#52).
//
// The whole RCA behind this branch is that on 2026-08-25 an operator
// could not see the admission cap collapsing. `diagnoseConnection`
// published `rate_limit_bytes_per_sec` — the CONFIGURED limit, the
// number that never moves — and reported one WARN, "tx failures/drops",
// for two unrelated faults with two unrelated remedies. These tests pin
// the replacement: the effective cap is named, and socket failure is
// distinguished from deliberate admission shedding.
//
// Pure logic, extracted from the node for the same reason
// computeGiveupDiagnostic was: deciding what to tell the operator needs
// no node, clock or socket.

#include "udp_bridge/connection_diagnostic.h"

#include <string>

#include <gtest/gtest.h>

namespace
{

using udp_bridge::computeConnectionDiagnostic;
using DS = diagnostic_msgs::msg::DiagnosticStatus;

constexpr uint32_t kConfigured = 1500000;
constexpr double kWarnAge = 5.0;
constexpr double kErrorAge = 10.0;

// A healthy, unthrottled connection with nothing dropped or failed.
udp_bridge::ConnectionDiagnostic healthy(uint32_t effective = kConfigured)
{
  return computeConnectionDiagnostic(kConfigured, effective,
                                     120000.0, 0.0, 0.0, 90000.0, 1.0,
                                     kWarnAge, kErrorAge);
}

}  // namespace

TEST(ConnectionDiagnostic, HealthyUnthrottledIsOkAndSaysNothingAboutACap)
{
  const auto d = healthy();
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_EQ(d.message.find("admission cap"), std::string::npos)
    << "An unthrottled connection must not mention a cap: " << d.message;
}

// The finding this branch exists for. A collapsed cap on an otherwise
// quiet link is the state that reads as "slow for no reason".
TEST(ConnectionDiagnostic, ThrottledCapIsNamedEvenWhenEverythingElseIsFine)
{
  const auto d = healthy(8192);
  EXPECT_EQ(d.level, DS::OK);
  EXPECT_NE(d.message.find("admission cap 8192 of 1500000 B/s"), std::string::npos)
    << "A collapsed admission cap must be visible in the summary an "
       "operator reads first, not only in a BridgeInfo field two "
       "subscriptions away: " << d.message;
}

// Shedding vs failing: the split the single "tx failures/drops" wording
// hid. A throttled connection sits in the shedding state continuously.
TEST(ConnectionDiagnostic, SheddingAtTheAdmissionCapIsNotReportedAsAFailure)
{
  const auto d = computeConnectionDiagnostic(kConfigured, 8192,
                                             8000.0, 0.0, 50000.0, 7000.0, 1.0,
                                             kWarnAge, kErrorAge);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_NE(d.message.find("shedding"), std::string::npos) << d.message;
  EXPECT_NE(d.message.find("8192 of 1500000 B/s"), std::string::npos)
    << "The cap being shed against must be named — that number IS the "
       "diagnosis: " << d.message;
  EXPECT_EQ(d.message.find("failure"), std::string::npos)
    << "Admission shedding is not a socket failure: " << d.message;
}

TEST(ConnectionDiagnostic, SheddingAtAnUncollapsedLimitNamesTheConfiguredLimit)
{
  const auto d = computeConnectionDiagnostic(kConfigured, kConfigured,
                                             1490000.0, 0.0, 50000.0, 1400000.0, 1.0,
                                             kWarnAge, kErrorAge);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_NE(d.message.find("configured rate limit 1500000 B/s"), std::string::npos)
    << "Shedding with no AIMD backoff in force is the operator's own "
       "configured limit doing its job, and must say so: " << d.message;
}

TEST(ConnectionDiagnostic, SocketFailuresAreReportedAsFailuresNotShedding)
{
  const auto d = computeConnectionDiagnostic(kConfigured, kConfigured,
                                             1000.0, 4000.0, 0.0, 900.0, 1.0,
                                             kWarnAge, kErrorAge);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_NE(d.message.find("tx failures"), std::string::npos) << d.message;
  EXPECT_EQ(d.message.find("shedding"), std::string::npos) << d.message;
}

// A socket failure outranks shedding: a throttled link that is ALSO
// failing to write to its socket has a fault the cap does not explain.
TEST(ConnectionDiagnostic, FailurePrecedesSheddingWhenBothArePresent)
{
  const auto d = computeConnectionDiagnostic(kConfigured, 8192,
                                             1000.0, 4000.0, 50000.0, 900.0, 1.0,
                                             kWarnAge, kErrorAge);
  EXPECT_EQ(d.level, DS::WARN);
  EXPECT_NE(d.message.find("tx failures"), std::string::npos) << d.message;
}

// Staleness precedence is unchanged behaviour and is pinned so the
// refactor that extracted this function cannot have altered it.
TEST(ConnectionDiagnostic, StaleReceiveOutranksEverythingElse)
{
  const auto d = computeConnectionDiagnostic(kConfigured, 8192,
                                             1000.0, 4000.0, 50000.0, 0.0, 30.0,
                                             kWarnAge, kErrorAge);
  EXPECT_EQ(d.level, DS::ERROR);
  EXPECT_NE(d.message.find("no rx for 30s"), std::string::npos) << d.message;
}

TEST(ConnectionDiagnostic, StaleReceiveBelowTheErrorThresholdWarnsButYieldsToTxFaults)
{
  const auto quiet = computeConnectionDiagnostic(kConfigured, kConfigured,
                                                 1000.0, 0.0, 0.0, 0.0, 7.0,
                                                 kWarnAge, kErrorAge);
  EXPECT_EQ(quiet.level, DS::WARN);
  EXPECT_NE(quiet.message.find("no rx for 7s"), std::string::npos) << quiet.message;

  const auto also_dropping = computeConnectionDiagnostic(kConfigured, 8192,
                                                         1000.0, 0.0, 50000.0, 0.0, 7.0,
                                                         kWarnAge, kErrorAge);
  EXPECT_NE(also_dropping.message.find("shedding"), std::string::npos)
    << also_dropping.message;
}

// An idle link has never received anything, which is not a stale link.
// The caller signals that with a negative age.
TEST(ConnectionDiagnostic, NeverReceivedIsNotStale)
{
  const auto d = computeConnectionDiagnostic(kConfigured, kConfigured,
                                             1000.0, 0.0, 0.0, 0.0, -1.0,
                                             kWarnAge, kErrorAge);
  EXPECT_EQ(d.level, DS::OK)
    << "A connection that has never received must not be reported stale: "
    << d.message;
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "udp_bridge/qos_resolution.h"
#include <gtest/gtest.h>
#include "rclcpp/rclcpp.hpp"

using udp_bridge::resolveDestinationPublisherQos;
using udp_bridge::resolveSourceSubscriptionQos;

namespace
{

// Convenience accessors for the underlying rmw_qos_profile_t fields.
// rclcpp::QoS exposes them via get_rmw_qos_profile().
rmw_qos_reliability_policy_t reliability_of(const rclcpp::QoS& qos)
{
  return qos.get_rmw_qos_profile().reliability;
}

rmw_qos_durability_policy_t durability_of(const rclcpp::QoS& qos)
{
  return qos.get_rmw_qos_profile().durability;
}

rmw_qos_history_policy_t history_of(const rclcpp::QoS& qos)
{
  return qos.get_rmw_qos_profile().history;
}

size_t depth_of(const rclcpp::QoS& qos)
{
  return qos.get_rmw_qos_profile().depth;
}

} // namespace

// --- resolveDestinationPublisherQos ----------------------------------------

TEST(QosResolution, DestinationDefault_EmptyEmptyZero)
{
  auto qos = resolveDestinationPublisherQos("", "", 0);
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE);
  EXPECT_EQ(durability_of(qos),  RMW_QOS_POLICY_DURABILITY_VOLATILE);
  EXPECT_EQ(history_of(qos),     RMW_QOS_POLICY_HISTORY_KEEP_LAST);
  EXPECT_EQ(depth_of(qos),       1u);
}

TEST(QosResolution, DestinationExplicitReliable)
{
  auto qos = resolveDestinationPublisherQos("reliable", "", 1);
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_RELIABLE);
}

TEST(QosResolution, DestinationExplicitBestEffort)
{
  auto qos = resolveDestinationPublisherQos("best_effort", "", 1);
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
}

TEST(QosResolution, DestinationExplicitBestAvailable)
{
  auto qos = resolveDestinationPublisherQos("best_available", "", 1);
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE);
}

TEST(QosResolution, DestinationUnrecognizedReliabilityFallsBackToDefault)
{
  // Per the contract: unrecognized strings should fall through to defaults
  // rather than erroring. Preserves graceful interop with future versions.
  auto qos = resolveDestinationPublisherQos("garbage", "", 1);
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE);
}

TEST(QosResolution, DestinationTransientLocal)
{
  auto qos = resolveDestinationPublisherQos("", "transient_local", 1);
  EXPECT_EQ(durability_of(qos), RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);
}

TEST(QosResolution, DestinationVolatileExplicit)
{
  auto qos = resolveDestinationPublisherQos("", "volatile", 1);
  EXPECT_EQ(durability_of(qos), RMW_QOS_POLICY_DURABILITY_VOLATILE);
}

TEST(QosResolution, DestinationUnrecognizedDurabilityFallsBackToVolatile)
{
  auto qos = resolveDestinationPublisherQos("", "garbage", 1);
  EXPECT_EQ(durability_of(qos), RMW_QOS_POLICY_DURABILITY_VOLATILE);
}

TEST(QosResolution, DestinationHistoryDepthHonored)
{
  auto qos = resolveDestinationPublisherQos("", "", 7);
  EXPECT_EQ(history_of(qos), RMW_QOS_POLICY_HISTORY_KEEP_LAST);
  EXPECT_EQ(depth_of(qos),   7u);
}

TEST(QosResolution, DestinationZeroDepthMeansDefaultOne)
{
  auto qos = resolveDestinationPublisherQos("", "", 0);
  EXPECT_EQ(depth_of(qos), 1u);
}

// --- Mismatched-pair compatibility (qos_design.md "Mismatched-pair compatibility") ---

TEST(QosResolution, OldSenderAllFieldsMissing_LooksLikeDefaults)
{
  // An old sender produces a MessageInternal with no QoS fields; on the
  // wire those deserialize as empty strings and 0. The receiver must
  // treat that as "use the package defaults" rather than failing.
  auto qos = resolveDestinationPublisherQos("", "", 0);
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE);
  EXPECT_EQ(durability_of(qos),  RMW_QOS_POLICY_DURABILITY_VOLATILE);
  EXPECT_EQ(depth_of(qos),       1u);
}

// --- resolveSourceSubscriptionQos ------------------------------------------

TEST(QosResolution, SourceDefaultIsBestEffortVolatile)
{
  auto qos = resolveSourceSubscriptionQos("", 10);
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
  EXPECT_EQ(durability_of(qos),  RMW_QOS_POLICY_DURABILITY_VOLATILE);
  EXPECT_EQ(depth_of(qos),       10u);
}

TEST(QosResolution, SourceTransientLocal)
{
  auto qos = resolveSourceSubscriptionQos("transient_local", 10);
  EXPECT_EQ(durability_of(qos), RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);
  // Reliability stays best_effort regardless of durability — the source-side
  // subscriber is universally compatible with any source publisher.
  EXPECT_EQ(reliability_of(qos), RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
}

TEST(QosResolution, SourceZeroQueueSizeMeansDefaultOne)
{
  auto qos = resolveSourceSubscriptionQos("", 0);
  EXPECT_EQ(depth_of(qos), 1u);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

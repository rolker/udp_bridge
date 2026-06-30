// Unit tests for RemoteNode::admitForPublish — the stale-packet gate
// added on the 2026-06-29 BizzyBoat deployment (operator saw old packets
// republished after newer ones on heartbeat/video/costmap, making the
// topic stamp jump backward). The gate keeps a per-destination-topic
// high-water mark of the wrapped packet_number last admitted and drops a
// decoded message whose number is strictly below it.
//
// These cases pin the documented contract:
//   - the first message for a topic is always admitted;
//   - a strictly-older packet_number is dropped (the late-resend case);
//   - an equal number is admitted (exact duplicates are filtered upstream
//     by received_packet_times_ before decode, so == reaching the gate is
//     not a duplicate — see admitForPublish's comment);
//   - a newer number advances the high-water mark;
//   - high-water marks are per-destination-topic and independent; and
//   - the marks reset on remote-restart detection, so a post-restart
//     packet (sender's numbers reset to 0) is NOT wrongly dropped. That
//     last case is the load-bearing one: without the clear in
//     update(BridgeInfo), every packet after a remote udp_bridge restart
//     would be rejected as stale and the link would go silent.

#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "rclcpp/rclcpp.hpp"

#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge_interfaces/msg/remote.hpp"
#include "udp_bridge/remote_node.h"

namespace
{

// Owns a node so RemoteNode has the NodeInterfaces it needs at
// construction. local_name_ is "test-local"; the remote is "test-remote".
class StaleGateFixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
    auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string node_name = std::string("test_stale_packet_gate_") + info->name();
    node_ = std::make_shared<rclcpp::Node>(node_name);
    remote_ = std::make_unique<udp_bridge::RemoteNode>(
      "test-remote", "test-local",
      udp_bridge::RemoteNode::NodeInterfaces(*node_));
  }

  // Drive update() twice to trip the remote-restart detection in
  // update(BridgeInfo): first a non-zero next_packet_number to seed
  // next_packet_number_, then 0 (0 < seed) which clears the gate's
  // high-water marks alongside the resend-tracking state.
  void triggerRemoteRestart()
  {
    udp_bridge::SourceInfo src;
    src.node_name = "test-remote";
    src.host = "127.0.0.1";
    src.port = 9999;

    udp_bridge_interfaces::msg::Remote remote_entry;
    remote_entry.name = "test-local";

    udp_bridge_interfaces::msg::BridgeInfo bi_seed;
    bi_seed.next_packet_number = 100;
    bi_seed.remotes.push_back(remote_entry);
    remote_->update(bi_seed, src);

    udp_bridge_interfaces::msg::BridgeInfo bi_restart;
    bi_restart.next_packet_number = 0;
    bi_restart.remotes.push_back(remote_entry);
    remote_->update(bi_restart, src);
  }

  std::shared_ptr<rclcpp::Node> node_;
  std::unique_ptr<udp_bridge::RemoteNode> remote_;
};

TEST_F(StaleGateFixture, FirstMessageForTopicAdmitted)
{
  EXPECT_TRUE(remote_->admitForPublish("topicA", 5));
}

TEST_F(StaleGateFixture, StrictlyOlderDropped)
{
  ASSERT_TRUE(remote_->admitForPublish("topicA", 5));
  EXPECT_FALSE(remote_->admitForPublish("topicA", 4));
  EXPECT_FALSE(remote_->admitForPublish("topicA", 0));
}

TEST_F(StaleGateFixture, EqualNumberAdmitted)
{
  ASSERT_TRUE(remote_->admitForPublish("topicA", 5));
  // == is not < , so it is admitted (exact duplicates never reach the
  // gate; they are filtered by received_packet_times_ before decode).
  EXPECT_TRUE(remote_->admitForPublish("topicA", 5));
}

TEST_F(StaleGateFixture, NewerNumberAdvancesHighWater)
{
  ASSERT_TRUE(remote_->admitForPublish("topicA", 5));
  EXPECT_TRUE(remote_->admitForPublish("topicA", 7));
  // High-water is now 7, so 6 (between the two) is stale.
  EXPECT_FALSE(remote_->admitForPublish("topicA", 6));
}

TEST_F(StaleGateFixture, HighWaterIsPerTopic)
{
  ASSERT_TRUE(remote_->admitForPublish("topicA", 10));
  // A different topic has its own high-water: first-seen 3 is admitted
  // even though topicA is far ahead.
  EXPECT_TRUE(remote_->admitForPublish("topicB", 3));
  // Each topic gates against only its own mark.
  EXPECT_FALSE(remote_->admitForPublish("topicA", 9));
  EXPECT_FALSE(remote_->admitForPublish("topicB", 2));
  EXPECT_TRUE(remote_->admitForPublish("topicB", 4));
}

TEST_F(StaleGateFixture, RemoteRestartResetsHighWater)
{
  // Advance topicA's high-water, then simulate a remote udp_bridge
  // restart (sender's packet numbers reset to 0).
  ASSERT_TRUE(remote_->admitForPublish("topicA", 100));
  ASSERT_FALSE(remote_->admitForPublish("topicA", 1))
    << "Sanity: 1 < 100 should be stale before the restart";

  triggerRemoteRestart();

  // After the restart the mark is cleared, so the first post-restart
  // packet (number 0) is admitted, not dropped as stale.
  EXPECT_TRUE(remote_->admitForPublish("topicA", 0))
    << "post-restart packet_number 0 was dropped — update(BridgeInfo) "
       "did not clear highest_published_packet_number_, so the gate "
       "rejects every packet after a remote restart and the link goes "
       "silent (remote_node.cpp restart-detection branch).";
  // And the gate resumes normally from the new baseline.
  EXPECT_TRUE(remote_->admitForPublish("topicA", 1));
  EXPECT_FALSE(remote_->admitForPublish("topicA", 0));
}

}  // namespace

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

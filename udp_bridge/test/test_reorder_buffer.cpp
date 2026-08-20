// Unit tests for RemoteNode::admitOrBuffer / flushExpiredBuffer — the
// reorder/jitter buffer (reorder_hold_window_ms, issue #35). Where the
// stale-packet gate (drop_stale_packets, #33) hard-drops any decoded
// packet whose packet_number is below the per-topic high-water mark, the
// reorder buffer instead holds a gap-opening packet briefly so an
// out-of-order gap-filler that arrives within the hold window can be
// published first, in order. It hard-drops only once the window expires.
//
// These cases pin the documented contract:
//   - a gap opens a Buffer decision WITHOUT advancing the high-water mark
//     (so the gap-filler can still be admitted);
//   - a gap filled within the window publishes both packets in order
//     (lower first, then the released held packet);
//   - window expiry releases the held packet on flush and advances the
//     mark, so a late-arriving lower packet is then dropped as stale;
//   - the at-most-one-per-topic slot: a second gap-opening packet flushes
//     the first;
//   - remote-restart detection clears the buffer alongside the high-water
//     marks; and
//   - the disabled path still routes through admitForPublish unchanged.
//
// Time is injected (fake rclcpp::Time via t_at) exactly like
// test_remote_node_resend.cpp, so the window-expiry cases carry no
// real-time sensitivity.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "rclcpp/rclcpp.hpp"

#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge_interfaces/msg/remote.hpp"
#include "udp_bridge/publish_queue.h"
#include "udp_bridge/remote_node.h"

namespace
{

constexpr rcl_clock_type_t kClock = RCL_ROS_TIME;

// A rclcpp::Time built from a scalar seconds offset (matches
// test_remote_node_resend.cpp's helper).
rclcpp::Time t_at(double s)
{
  return rclcpp::Time(static_cast<int64_t>(s * 1e9), kClock);
}

// Build a PublishItem tagged with its packet number in `datatype`, so a
// test can assert the order items come back in without decoding payloads.
udp_bridge::PublishItem makeItem(uint64_t packet_number,
                                 const std::string& topic = "topicA")
{
  udp_bridge::PublishItem item;
  item.topic = topic;
  item.datatype = std::to_string(packet_number);
  return item;
}

// Extract the packet-number tags from a to_publish / released vector,
// preserving order.
std::vector<uint64_t> tags(const std::vector<udp_bridge::PublishItem>& items)
{
  std::vector<uint64_t> out;
  out.reserve(items.size());
  for(const auto& i : items)
    out.push_back(std::stoull(i.datatype));
  return out;
}

using AdmitDecision = udp_bridge::RemoteNode::AdmitDecision;

// Owns a node so RemoteNode has the NodeInterfaces it needs at
// construction. Mirrors the StaleGateFixture, incl. triggerRemoteRestart.
class ReorderBufferFixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
    auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string node_name = std::string("test_reorder_buffer_") + info->name();
    node_ = std::make_shared<rclcpp::Node>(node_name);
    remote_ = std::make_unique<udp_bridge::RemoteNode>(
      "test-remote", "test-local",
      udp_bridge::RemoteNode::NodeInterfaces(*node_));
  }

  // Drive update() twice to trip the remote-restart detection in
  // update(BridgeInfo): a non-zero next_packet_number to seed, then 0.
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

  // Convenience wrapper: admitOrBuffer at a fixed arrival time.
  udp_bridge::RemoteNode::AdmitResult admit(uint64_t n, double at = 1.0,
                                            const std::string& topic = "topicA")
  {
    return remote_->admitOrBuffer(topic, n, makeItem(n, topic), t_at(at));
  }

  std::shared_ptr<rclcpp::Node> node_;
  std::unique_ptr<udp_bridge::RemoteNode> remote_;
};

// First packet seen for a topic is always admitted (matches admitForPublish).
TEST_F(ReorderBufferFixture, FirstSeenAdmitted)
{
  auto r = admit(1);
  EXPECT_EQ(r.decision, AdmitDecision::Admit);
  EXPECT_EQ(tags(r.to_publish), (std::vector<uint64_t>{1}));
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 1);
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), -1);
}

// Contiguous packet (high-water + 1) admits immediately, no buffering.
TEST_F(ReorderBufferFixture, ContiguousAdmitted)
{
  ASSERT_EQ(admit(1).decision, AdmitDecision::Admit);
  auto r = admit(2);
  EXPECT_EQ(r.decision, AdmitDecision::Admit);
  EXPECT_EQ(tags(r.to_publish), (std::vector<uint64_t>{2}));
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 2);
}

// A gap (>= high-water + 2) buffers the packet WITHOUT advancing the mark.
TEST_F(ReorderBufferFixture, GapBufferedWithoutHighWaterAdvance)
{
  ASSERT_EQ(admit(1).decision, AdmitDecision::Admit);

  auto r = admit(3);  // 2 is missing
  EXPECT_EQ(r.decision, AdmitDecision::Buffer);
  EXPECT_TRUE(r.to_publish.empty());
  // The load-bearing assertion: the mark stayed at 1, so the gap-filler
  // (packet 2) can still be admitted.
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 1);
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), 3);
}

// Gap filled within the window: the filler and the released held packet
// are both published, lower number first.
TEST_F(ReorderBufferFixture, GapFilledInWindowPublishedInOrder)
{
  ASSERT_EQ(admit(1).decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(3).decision, AdmitDecision::Buffer);

  auto r = admit(2);  // closes the gap
  EXPECT_EQ(r.decision, AdmitDecision::Admit);
  EXPECT_EQ(tags(r.to_publish), (std::vector<uint64_t>{2, 3}));
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), -1);
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 3);
}

// A gap-filler that only partially closes the gap is admitted; the held
// packet stays buffered (still a gap above the filler).
TEST_F(ReorderBufferFixture, PartialGapFillKeepsBuffer)
{
  ASSERT_EQ(admit(1).decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(4).decision, AdmitDecision::Buffer);  // 2,3 missing

  auto r = admit(2);
  EXPECT_EQ(r.decision, AdmitDecision::Admit);
  EXPECT_EQ(tags(r.to_publish), (std::vector<uint64_t>{2}));
  // 4 is not contiguous with 2, so it stays held; mark advanced only to 2.
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), 4);
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 2);
}

// Window expiry: flushExpiredBuffer releases the held packet and advances
// the mark, so a late-arriving lower packet is then dropped as stale.
TEST_F(ReorderBufferFixture, WindowExpiryReleasesHeldPacket)
{
  const auto window = rclcpp::Duration::from_seconds(0.050);  // 50 ms
  ASSERT_EQ(admit(1, 1.000).decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(3, 1.000).decision, AdmitDecision::Buffer);

  // Still inside the window (20 ms later): nothing released.
  auto early = remote_->flushExpiredBuffer(t_at(1.020), window);
  EXPECT_TRUE(early.empty());
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), 3);

  // Past the window (100 ms later): the held packet is released and the
  // mark advances to it.
  auto released = remote_->flushExpiredBuffer(t_at(1.100), window);
  EXPECT_EQ(tags(released), (std::vector<uint64_t>{3}));
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), -1);
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 3);

  // The late gap-filler now arrives — too late; dropped as stale.
  auto late = admit(2, 1.110);
  EXPECT_EQ(late.decision, AdmitDecision::Drop);
  EXPECT_TRUE(late.to_publish.empty());
}

// At-most-one overflow: a second gap-opening packet flushes the first.
TEST_F(ReorderBufferFixture, AtMostOneOverflowFlushesFirst)
{
  ASSERT_EQ(admit(1).decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(3).decision, AdmitDecision::Buffer);  // buffered = 3

  // Packet 5 is newer than the held 3 and still leaves a gap (4 missing):
  // 3 is flushed for publish, 5 takes the slot, mark advances to 3.
  auto r = admit(5);
  EXPECT_EQ(r.decision, AdmitDecision::Buffer);
  EXPECT_EQ(tags(r.to_publish), (std::vector<uint64_t>{3}));
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), 5);
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 3);
}

// Overflow where the newer packet is contiguous with the flushed one:
// both go out in order and nothing stays buffered.
TEST_F(ReorderBufferFixture, OverflowContiguousPublishesBoth)
{
  ASSERT_EQ(admit(1).decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(3).decision, AdmitDecision::Buffer);

  auto r = admit(4);  // 4 > buffered 3, and 4 == 3 + 1
  EXPECT_EQ(r.decision, AdmitDecision::Admit);
  EXPECT_EQ(tags(r.to_publish), (std::vector<uint64_t>{3, 4}));
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), -1);
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 4);
}

// A packet strictly below the mark is dropped even with a packet buffered;
// the buffer is left untouched.
TEST_F(ReorderBufferFixture, StaleBelowMarkDroppedBufferUntouched)
{
  ASSERT_EQ(admit(5).decision, AdmitDecision::Admit);  // mark = 5
  ASSERT_EQ(admit(8).decision, AdmitDecision::Buffer); // buffered = 8

  auto r = admit(4);  // 4 < mark 5
  EXPECT_EQ(r.decision, AdmitDecision::Drop);
  EXPECT_TRUE(r.to_publish.empty());
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), 8);
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), 5);
}

// Buffers are independent per destination topic.
TEST_F(ReorderBufferFixture, BufferIsPerTopic)
{
  ASSERT_EQ(admit(1, 1.0, "topicA").decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(3, 1.0, "topicA").decision, AdmitDecision::Buffer);

  // topicB is untouched by topicA's state — first-seen admits.
  auto r = admit(10, 1.0, "topicB");
  EXPECT_EQ(r.decision, AdmitDecision::Admit);
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), 3);
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicB"), -1);
}

// Remote-restart detection clears the buffer alongside the high-water
// marks, so a held pre-restart packet does not linger.
TEST_F(ReorderBufferFixture, RemoteRestartClearsBuffer)
{
  ASSERT_EQ(admit(1).decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(3).decision, AdmitDecision::Buffer);
  ASSERT_EQ(remote_->bufferedPacketNumberForTest("topicA"), 3);

  triggerRemoteRestart();

  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), -1)
    << "held packet survived a remote restart — update(BridgeInfo) did "
       "not clear reorder_buffer_";
  EXPECT_EQ(remote_->highestPublishedForTest("topicA"), -1);

  // The link resumes from a clean baseline: post-restart packet 0 admits.
  auto r = admit(0);
  EXPECT_EQ(r.decision, AdmitDecision::Admit);
  EXPECT_EQ(tags(r.to_publish), (std::vector<uint64_t>{0}));
}

// Disabled path (reorder_hold_window_ms == 0): decodeData routes sequenced
// packets through admitForPublish, unchanged from before #35. This pins
// that the stale-gate semantics the disabled path depends on still hold.
TEST_F(ReorderBufferFixture, DisabledPathUsesAdmitForPublishUnchanged)
{
  EXPECT_TRUE(remote_->admitForPublish("topicA", 5));
  EXPECT_FALSE(remote_->admitForPublish("topicA", 4));  // strictly older dropped
  EXPECT_TRUE(remote_->admitForPublish("topicA", 5));   // == admitted
  EXPECT_TRUE(remote_->admitForPublish("topicA", 7));   // newer advances
  EXPECT_FALSE(remote_->admitForPublish("topicA", 6));  // now stale
  // The disabled path never touches the reorder buffer.
  EXPECT_EQ(remote_->bufferedPacketNumberForTest("topicA"), -1);
}

// Observability counters (review follow-up): a Buffer decision bumps the
// cumulative buffered total; only a window-expiry release bumps the
// expired total — an in-window gap-filler release does not.
TEST_F(ReorderBufferFixture, ObservabilityCountersTrackBufferAndExpiry)
{
  const auto window = rclcpp::Duration::from_seconds(0.050);  // 50 ms
  EXPECT_EQ(remote_->reorderBufferOccupancy(), 0u);
  EXPECT_EQ(remote_->reorderBufferedTotal(), 0u);
  EXPECT_EQ(remote_->reorderExpiredTotal(), 0u);

  // Gap filled in-window: buffered_total 1, expired stays 0.
  ASSERT_EQ(admit(1, 1.000).decision, AdmitDecision::Admit);
  ASSERT_EQ(admit(3, 1.000).decision, AdmitDecision::Buffer);
  EXPECT_EQ(remote_->reorderBufferOccupancy(), 1u);
  EXPECT_EQ(remote_->reorderBufferedTotal(), 1u);
  ASSERT_EQ(admit(2, 1.010).decision, AdmitDecision::Admit);  // releases 3
  EXPECT_EQ(remote_->reorderBufferOccupancy(), 0u);
  EXPECT_EQ(remote_->reorderExpiredTotal(), 0u);

  // Gap never filled: window expiry bumps expired_total.
  ASSERT_EQ(admit(5, 2.000).decision, AdmitDecision::Buffer);
  EXPECT_EQ(remote_->reorderBufferedTotal(), 2u);
  auto released = remote_->flushExpiredBuffer(t_at(2.100), window);
  EXPECT_EQ(tags(released), (std::vector<uint64_t>{5}));
  EXPECT_EQ(remote_->reorderBufferOccupancy(), 0u);
  EXPECT_EQ(remote_->reorderExpiredTotal(), 1u);
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

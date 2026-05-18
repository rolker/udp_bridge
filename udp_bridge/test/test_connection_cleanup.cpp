// Unit test for Connection::cleanup_sent_packets — the sender-side
// of the kSentPacketTTL contract.
//
// Pairs with the receiver-side give-up test in
// test_remote_node_resend.cpp. Together they pin both ends of failure
// mode D from issue #9: the sender forgets at kSentPacketTTL; the
// receiver gives up at kSentPacketTTL. The two cutoffs must align
// (kReceiveHistoryWindow <= kSentPacketTTL, enforced by static_assert
// in resend_constants.h) so the receiver never requests packets the
// sender has already forgotten.
//
// This test exercises only the sender-side eviction. It does not
// re-test the alignment invariant — that's pinned at compile time.

#include <cstdint>

#include <gtest/gtest.h>

#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/resend_constants.h"

namespace
{

constexpr rcl_clock_type_t kClock = RCL_ROS_TIME;

rclcpp::Time t_at(double s)
{
  return rclcpp::Time(static_cast<int64_t>(s * 1e9), kClock);
}

}  // namespace

// Boundary contract for cleanup_sent_packets: entries with
// `timestamp < cutoff_time` are evicted; entries with
// `timestamp >= cutoff_time` are retained. A packet sent at exactly
// the cutoff time stays in the buffer; one sent epsilon before it is
// evicted.
//
// Production caller (UDPBridge::cleanupSentPackets) computes
// cutoff_time = now - kSentPacketTTL, so this contract translates to:
// a packet stays available for resend for at least kSentPacketTTL
// after its send time.
TEST(ConnectionCleanup, BoundaryEvictionIsStrictLessThan)
{
  udp_bridge::Connection conn("test-cleanup", "127.0.0.1", 9999, "", 0);

  // Three packets at distinct times around an arbitrary anchor T = 5s.
  conn.recordSentPacketForTest(1, t_at(4.999));  // before cutoff
  conn.recordSentPacketForTest(2, t_at(5.000));  // exactly at cutoff
  conn.recordSentPacketForTest(3, t_at(5.001));  // after cutoff
  ASSERT_EQ(conn.sentPacketCountForTest(), 3u);

  // Eviction cutoff of T = 5s. Packet 1 (timestamp=4.999) is < cutoff
  // and must be evicted; packet 2 (timestamp=5.000) is == cutoff and
  // must be retained (strict <); packet 3 (5.001) > cutoff retained.
  conn.cleanup_sent_packets(t_at(5.000));
  EXPECT_EQ(conn.sentPacketCountForTest(), 2u)
    << "Packet at timestamp == cutoff should be retained "
       "(strict-less-than comparison)";

  // Idempotent: re-running cleanup at the same cutoff changes nothing.
  conn.cleanup_sent_packets(t_at(5.000));
  EXPECT_EQ(conn.sentPacketCountForTest(), 2u);

  // Advance cutoff to t=5.001. Packet 2 (timestamp=5.000) is now <
  // cutoff and must be evicted; packet 3 (timestamp=5.001) is still
  // == cutoff and must be retained.
  conn.cleanup_sent_packets(t_at(5.001));
  EXPECT_EQ(conn.sentPacketCountForTest(), 1u);

  // Advance cutoff to t=5.002. Packet 3 (timestamp=5.001) is now <
  // cutoff and must be evicted. Buffer is empty.
  conn.cleanup_sent_packets(t_at(5.002));
  EXPECT_EQ(conn.sentPacketCountForTest(), 0u);
}

// Sanity: a no-op cleanup when the buffer is empty should succeed
// without altering anything.
TEST(ConnectionCleanup, NoopCleanupOnEmptyBuffer)
{
  udp_bridge::Connection conn("test-empty", "127.0.0.1", 9999, "", 0);
  conn.cleanup_sent_packets(t_at(100.0));
  SUCCEED();
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

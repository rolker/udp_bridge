// Regression test for the unaddressed-connection short-circuit in
// Connection::send.
//
// A connection whose host has never been advertised has nowhere to send. Before
// this guard, the batch send() still copied every packet into a WrappedPacket
// and filed it in sent_packets_ so a peer could request a resend -- for a peer
// that had never been heard from and therefore could never request anything.
// Every entry could only ever be evicted by cleanup.
//
// Measured on BizzyBoat 2026-08-20: the operator station was reachable only
// over vpn and cell and never advertised a wifi address, so the configured wifi
// connection sat at host='' with the full topic list pointed at it -- roughly
// 1.8 MB/s of copies, map churn and resend bookkeeping holding a CPU core at
// ~96% while delivering exactly zero bytes.

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/types.h"
#include "udp_bridge/wrapped_packet.h"

namespace
{

class UnaddressedConnection : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
  }

  std::vector<udp_bridge::WrappedPacket> makePackets(int count, std::size_t size,
                                                     rclcpp::Time now)
  {
    std::vector<udp_bridge::WrappedPacket> packets;
    packets.reserve(count);
    for(int i = 0; i < count; ++i)
    {
      std::vector<uint8_t> data(size, 0xCD);
      packets.push_back(udp_bridge::WrappedPacket(static_cast<uint64_t>(i), data, now));
    }
    return packets;
  }
};

// An empty host is exactly what a connection looks like when the remote has
// never advertised an address for it -- the state the BizzyBoat wifi connection
// was stuck in.
TEST_F(UnaddressedConnection, DoesNotFilePacketsForResend)
{
  udp_bridge::Connection conn("no-address", "", 0, "", 0);
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto now = clock.now();

  ASSERT_EQ(0u, conn.sent_packet_count_for_test());

  const auto packets = makePackets(50, 1000, now);
  const auto result = conn.send(packets, -1, "remote", false, now);

  // Reported as failed, not success and not merely rate-limited: there is no
  // destination, and the caller must be able to tell that apart from a link
  // that is simply saturated.
  EXPECT_EQ(udp_bridge::SendResult::failed, result);

  // The point of the fix: nothing is queued for a resend that can never be
  // requested. Before the guard this was 50.
  EXPECT_EQ(0u, conn.sent_packet_count_for_test());

  // The other half of the contract: the failure stays VISIBLE. The short-
  // circuited packets must land in the send statistics as failed bytes so a
  // misconfigured or out-of-range link cannot become an invisible one.
  auto rates = conn.data_sent_rate(now, udp_bridge::PacketSendCategory::message);
  EXPECT_GT(rates.failed_bytes_per_second, 0.0f);
}

TEST_F(UnaddressedConnection, StaysCheapAcrossRepeatedSends)
{
  udp_bridge::Connection conn("no-address-repeat", "", 0, "", 0);
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto now = clock.now();

  // The failure mode was unbounded growth over hours of operation, not a
  // single bad send. Hammer it and confirm the buffer never starts filling.
  for(int round = 0; round < 20; ++round)
  {
    const auto packets = makePackets(25, 500, now);
    EXPECT_EQ(udp_bridge::SendResult::failed,
              conn.send(packets, -1, "remote", false, now));
    ASSERT_EQ(0u, conn.sent_packet_count_for_test())
      << "resend buffer grew on round " << round;
  }
}

// Overhead traffic takes the same path and must be short-circuited too --
// otherwise the connection keeps paying for keepalives and advertisements.
TEST_F(UnaddressedConnection, ShortCircuitsOverheadTraffic)
{
  udp_bridge::Connection conn("no-address-overhead", "", 0, "", 0);
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto now = clock.now();

  const auto packets = makePackets(10, 200, now);
  EXPECT_EQ(udp_bridge::SendResult::failed,
            conn.send(packets, -1, "remote", true, now));
  EXPECT_EQ(0u, conn.sent_packet_count_for_test());

  // Overhead traffic is recorded under its own category — visible there too.
  auto rates = conn.data_sent_rate(now, udp_bridge::PacketSendCategory::overhead);
  EXPECT_GT(rates.failed_bytes_per_second, 0.0f);
}

// The guard must key on "no resolved address", not on the connection being new.
// Once an address is advertised the connection must resume filing packets, or
// resends would silently stop working for every link.
TEST_F(UnaddressedConnection, ResumesFilingOnceAnAddressIsAdvertised)
{
  udp_bridge::Connection conn("late-address", "", 0, "", 0);
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto now = clock.now();

  const auto before = makePackets(5, 100, now);
  conn.send(before, -1, "remote", false, now);
  ASSERT_EQ(0u, conn.sent_packet_count_for_test());

  // The remote finally advertises itself (the wifi-comes-into-range case).
  // A real socket is required from here on: with an address resolved, send()
  // no longer short-circuits and goes on to poll()/sendto().
  const int sock = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sock, 0) << "could not open a UDP socket";
  conn.setHostAndPort("127.0.0.1", 9999);
  conn.setRateLimit(1000000);

  const auto after = makePackets(5, 100, now);
  conn.send(after, sock, "remote", false, now);
  close(sock);

  // Filing resumed: the packets are retained so the peer can request a resend.
  EXPECT_EQ(5u, conn.sent_packet_count_for_test());
}

}  // namespace

// Unit tests for udp_bridge::MessageStatistics (issue #51).
//
// MessageStatistics::get() emits one TopicStatistics row per
// (destination_node, connection_id) pair it has seen. Both senders
// deliberately seed an empty pair — `send_results[""][""]` — once per
// message, which produces the row with `destination_node == ""`: the
// topic's AGGREGATE message count, independent of how many destinations
// the message actually reached (or whether any did).
//
// That seed is easy to omit, and omitting it is silent: the per-destination
// rows still appear and look right, while the aggregate row quietly counts
// only the traffic from whichever sender did seed. On a relay hub that
// means `~/topic_statistics` reporting a relayed topic's rate as if the
// hub's own publishers were the only source. These tests pin the seed's
// observable consequence, so the relay path (UDPBridge::relayToOtherRemotes)
// and the local path (UDPBridge::callback) cannot drift apart on it.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "udp_bridge/statistics.h"

using udp_bridge::MessageSizeData;
using udp_bridge::MessageStatistics;
using udp_bridge::SendResult;

namespace
{

const rclcpp::Time kT0(1000, 0, RCL_ROS_TIME);

// One recorded send to a named destination, as either sender records it
// after send() returns.
MessageSizeData perDestination(const std::string& remote,
                               const std::string& connection,
                               int message_size,
                               const rclcpp::Time& stamp)
{
  MessageSizeData data;
  data.message_size = message_size;
  data.sent_size = message_size;
  data.timestamp = stamp;
  data.send_results[remote][connection] = SendResult::success;
  return data;
}

// The once-per-message aggregate seed both senders take under
// subscribers_mutex_ before the send loop.
MessageSizeData aggregateSeed(int message_size, const rclcpp::Time& stamp)
{
  MessageSizeData data;
  data.message_size = message_size;
  data.timestamp = stamp;
  data.send_results[""][""];
  return data;
}

int countRowsFor(std::vector<udp_bridge_interfaces::msg::TopicStatistics>& rows,
                 const std::string& destination_node)
{
  int count = 0;
  for(const auto& row: rows)
    if(row.destination_node == destination_node)
      ++count;
  return count;
}

} // namespace

// Without the seed there is simply no aggregate row — the omission does not
// produce a wrong number, it produces a missing one, which is why it can
// survive a review of the per-destination numbers.
TEST(MessageStatisticsTest, NoAggregateRowWithoutTheSeed)
{
  MessageStatistics statistics;
  statistics.add(perDestination("operator_1", "wan", 100, kT0));
  statistics.add(perDestination("operator_1", "wan", 100,
                                kT0 + rclcpp::Duration::from_seconds(1.0)));

  auto rows = statistics.get();
  EXPECT_EQ(countRowsFor(rows, ""), 0);
  EXPECT_EQ(countRowsFor(rows, "operator_1"), 1);
}

// Seeded once per message — the contract both senders follow — the
// aggregate row counts messages, not destinations: two messages that each
// went to two remotes are two aggregate samples, not four.
TEST(MessageStatisticsTest, AggregateRowCountsMessagesNotDestinations)
{
  MessageStatistics statistics;
  const auto t1 = kT0 + rclcpp::Duration::from_seconds(1.0);

  for(const auto& stamp: {kT0, t1})
  {
    statistics.add(aggregateSeed(100, stamp));
    statistics.add(perDestination("operator_1", "wan", 100, stamp));
    statistics.add(perDestination("operator_2", "wan", 100, stamp));
  }

  auto rows = statistics.get();
  ASSERT_EQ(countRowsFor(rows, ""), 1);
  EXPECT_EQ(countRowsFor(rows, "operator_1"), 1);
  EXPECT_EQ(countRowsFor(rows, "operator_2"), 1);

  // Two samples one second apart, with get()'s "account for the time until
  // the next sample" correction: 2 / (1.0 * 3/2) messages per second. The
  // exact value matters less than that it equals the per-destination rate —
  // if the aggregate were counting destination rows it would be double.
  udp_bridge_interfaces::msg::TopicStatistics aggregate;
  udp_bridge_interfaces::msg::TopicStatistics per_destination;
  for(const auto& row: rows)
  {
    if(row.destination_node.empty())
      aggregate = row;
    else if(row.destination_node == "operator_1")
      per_destination = row;
  }
  EXPECT_FLOAT_EQ(aggregate.messages_per_second, per_destination.messages_per_second);
}

// The relay case that motivated this: a topic reaching a remote only by
// relay must still show up in the aggregate row. Modelled as two messages
// the hub forwarded (each seeded, each sent to one remote) with no local
// publication at all — the aggregate must count both, not zero.
TEST(MessageStatisticsTest, RelayedMessagesReachTheAggregateRow)
{
  MessageStatistics statistics;
  const auto t1 = kT0 + rclcpp::Duration::from_seconds(1.0);

  for(const auto& stamp: {kT0, t1})
  {
    statistics.add(aggregateSeed(250, stamp));
    statistics.add(perDestination("operator_1", "wan", 250, stamp));
  }

  auto rows = statistics.get();
  ASSERT_EQ(countRowsFor(rows, ""), 1);
  for(const auto& row: rows)
    if(row.destination_node.empty())
    {
      EXPECT_GT(row.messages_per_second, 0.0f)
        << "relayed traffic must not be invisible in the topic's aggregate";
      EXPECT_GT(row.message_bytes_per_second, 0.0f);
    }
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

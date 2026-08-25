// Per-topic send caps: one topic must not be able to crowd out the rest
// of a connection.
//
// Connection::send meters the connection-level `maximum_bytes_per_second`
// first-come-first-served through can_send, with no notion of which topic
// a packet belongs to. So a single topic can consume the whole admitted
// budget, and every other topic on that connection is shed as a side
// effect. Measured on BizzyBoat 2026-08-25: during an admission-control
// collapse all four camera streams were each delivering about a third of
// their bytes at the same time as a bulk topic, with nothing in the
// bridge able to prefer one over another.
//
// The cap is enforced in destination_selection.h, in the same pass that
// already applies the per-topic `period` rule — the one place BOTH
// senders meter (UDPBridge::callback for a locally published message and
// UDPBridge::relayToOtherRemotes for a forwarded one). Putting it
// anywhere else would have meant a second implementation for relayed
// traffic, and the two would drift.
//
// Pinned contract points:
//
//   ZeroCapIsUnlimited          — absent or 0 leaves today's behaviour
//                                 exactly as it was, so no existing
//                                 config changes.
//   CappedTopicShedsWhileSiblingGetsThrough — the whole point: a topic
//                                 over its own cap is dropped while a
//                                 sibling under its cap on the SAME
//                                 connection is unaffected.
//   OverBudgetIsReportedNotSilent — the shed destinations come back in
//                                 `over_budget`, which is what makes the
//                                 drop visible in TopicStatistics.
//   ShedConnectionKeepsItsSendState — a connection that was dropped must
//                                 not have last_sent_time stamped:
//                                 nothing was sent to it, so its `period`
//                                 clock must not move.
//   BudgetDrainsAtTheCap        — the bucket drains continuously, so the
//                                 sustained rate converges on the cap
//                                 rather than on a per-window sawtooth.
//   OversizedMessageIsNotSilencedForever — a message larger than one
//                                 second of the cap is admitted from an
//                                 empty bucket. Without this the topic
//                                 would go permanently silent instead of
//                                 slow — a far worse failure for a camera
//                                 stream than a low frame rate.
//   BackwardsClockResetsTheBucket — sim time and NTP corrections both
//                                 step backwards; the bucket must not end
//                                 up undrainable.
//   PeriodGateRunsFirst         — a connection the `period` rule already
//                                 held back is not charged for a message
//                                 it was never going to receive.
//   DropRecordLandsInTopicStatistics — a per-topic drop shows up in the
//                                 topic's `send` DataRates for that
//                                 (remote, connection), the way a
//                                 connection-level drop does.
//   Node-level: ConfiguredCapReachesTheRoutingTable,
//               RuntimeSetChangesTheCap, RuntimeZeroRemovesTheCap,
//               RuntimeNegativeRejected.

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lifecycle_msgs/msg/state.hpp"
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/destination_selection.h"
#include "udp_bridge/statistics.h"
#include "udp_bridge/types.h"
#include "udp_bridge/udp_bridge.h"

using udp_bridge::ConnectionRateInfo;
using udp_bridge::MessageStatistics;
using udp_bridge::RemoteDetails;
using udp_bridge::SendResult;
using udp_bridge::admitAgainstTopicByteCap;
using udp_bridge::overBudgetDropRecord;
using udp_bridge::selectRateLimitedConnections;

namespace
{

const rclcpp::Time kT0(1000, 0, RCL_ROS_TIME);

rclcpp::Time at(double seconds_after_t0)
{
  return kT0 + rclcpp::Duration::from_seconds(seconds_after_t0);
}

// One remote listing `topic` over one connection, with an optional
// per-topic cap and period.
RemoteDetails remote(const std::string& destination_topic,
                     const std::string& connection_id,
                     uint32_t maximum_bytes_per_second = 0,
                     float period = 0.0f)
{
  RemoteDetails details;
  details.destination_topic = destination_topic;
  ConnectionRateInfo rate;
  rate.period = period;
  rate.last_sent_time = rclcpp::Time(0, 0, RCL_ROS_TIME);
  rate.maximum_bytes_per_second = maximum_bytes_per_second;
  details.connection_rates[connection_id] = rate;
  return details;
}

}  // namespace

// ---------------------------------------------------------------------
// The budget itself. Pure, no node, no clock.
// ---------------------------------------------------------------------

TEST(TopicByteCap, ZeroCapIsUnlimited)
{
  ConnectionRateInfo rate;
  rate.period = 0.0f;
  rate.maximum_bytes_per_second = 0;
  for(int i = 0; i < 1000; ++i)
    EXPECT_TRUE(admitAgainstTopicByteCap(rate, 100000, kT0))
      << "an absent or zero cap must not shed anything (iteration " << i << ")";
  EXPECT_EQ(rate.budget_used_bytes, 0.0)
    << "an unlimited topic must not accumulate budget state at all";
}

TEST(TopicByteCap, AdmitsUpToTheCapThenSheds)
{
  ConnectionRateInfo rate;
  rate.maximum_bytes_per_second = 1000;
  rate.period = 0.0f;
  EXPECT_TRUE(admitAgainstTopicByteCap(rate, 600, kT0));
  // 600 + 600 exceeds one second of the cap, and the bucket is not empty.
  EXPECT_FALSE(admitAgainstTopicByteCap(rate, 600, kT0));
  // The refused message is not charged, so a smaller one still fits.
  EXPECT_TRUE(admitAgainstTopicByteCap(rate, 300, kT0));
  EXPECT_FALSE(admitAgainstTopicByteCap(rate, 200, kT0));
}

TEST(TopicByteCap, BudgetDrainsAtTheCap)
{
  // The bucket is one second of the cap deep and drains continuously, so
  // the room available at any instant is exactly what has drained since
  // the last charge. That is the same instantaneous allowance the
  // connection-level strict 1-second window gives.
  ConnectionRateInfo rate;
  rate.maximum_bytes_per_second = 1000;
  rate.period = 0.0f;
  ASSERT_TRUE(admitAgainstTopicByteCap(rate, 1000, kT0));

  // 0.1 s drains 100 of the 1000 charged bytes, so 100 bytes of room.
  EXPECT_FALSE(admitAgainstTopicByteCap(rate, 200, at(0.1)))
    << "200 bytes does not fit in the 100 bytes drained so far";
  EXPECT_TRUE(admitAgainstTopicByteCap(rate, 100, at(0.1)))
    << "100 bytes exactly fills the room that has drained";

  // A full second later the whole charge has drained.
  EXPECT_TRUE(admitAgainstTopicByteCap(rate, 1000, at(1.1)));
  // ...and the bucket is immediately full again.
  EXPECT_FALSE(admitAgainstTopicByteCap(rate, 1000, at(1.1)));
}

TEST(TopicByteCap, OversizedMessageIsNotSilencedForever)
{
  // A 3 kB message against a 1 kB/s cap. A strict "must fit in the
  // bucket" rule would refuse it forever and the topic would go silent
  // rather than slow.
  ConnectionRateInfo rate;
  rate.maximum_bytes_per_second = 1000;
  rate.period = 0.0f;
  EXPECT_TRUE(admitAgainstTopicByteCap(rate, 3000, kT0))
    << "an empty bucket admits any single message, however large";
  EXPECT_FALSE(admitAgainstTopicByteCap(rate, 3000, at(1.0)))
    << "the overdraft still has 2000 bytes to drain";
  EXPECT_TRUE(admitAgainstTopicByteCap(rate, 3000, at(3.0)))
    << "after 3 s the 3000-byte charge has drained; sustained rate is the cap";
}

TEST(TopicByteCap, BackwardsClockResetsTheBucket)
{
  ConnectionRateInfo rate;
  rate.maximum_bytes_per_second = 1000;
  rate.period = 0.0f;
  ASSERT_TRUE(admitAgainstTopicByteCap(rate, 1000, at(10.0)));
  EXPECT_FALSE(admitAgainstTopicByteCap(rate, 1000, at(10.0)));
  // Clock steps backwards (sim time, NTP correction). Forget the charge
  // rather than computing a negative drain, which would leave a bucket
  // that can never empty.
  EXPECT_TRUE(admitAgainstTopicByteCap(rate, 1000, at(5.0)));
}

// ---------------------------------------------------------------------
// Enforcement inside the shared selector.
// ---------------------------------------------------------------------

TEST(TopicRateCaps, CappedTopicShedsWhileSiblingGetsThrough)
{
  // Two topics on the SAME connection to the same remote: one bulk topic
  // capped at 1 kB/s, one status topic uncapped. This is the shape of the
  // field problem — the bulk topic used to take the whole connection.
  std::map<std::string, RemoteDetails> bulk;
  bulk["operator"] = remote("/bulk", "vpn", 1000);
  std::map<std::string, RemoteDetails> status;
  status["operator"] = remote("/status", "vpn");

  // First 5 kB message on each: both go (an empty bucket admits any one
  // message).
  EXPECT_EQ(selectRateLimitedConnections(bulk, kT0, 5000).due.count("operator"), 1u);
  EXPECT_EQ(selectRateLimitedConnections(status, kT0, 5000).due.count("operator"), 1u);

  // Second message, same instant. The bulk topic is over its own cap; the
  // status topic has none and is unaffected by the bulk topic's spending.
  auto bulk_second = selectRateLimitedConnections(bulk, kT0, 5000);
  EXPECT_TRUE(bulk_second.due.empty())
    << "a topic over its own cap must be shed";
  EXPECT_EQ(bulk_second.over_budget.count("operator"), 1u)
    << "and reported, so the drop is accounted rather than silent";

  auto status_second = selectRateLimitedConnections(status, kT0, 5000);
  EXPECT_EQ(status_second.due.count("operator"), 1u)
    << "an uncapped sibling on the same connection must still get through — "
       "this is the whole purpose of a per-topic cap";
  EXPECT_TRUE(status_second.over_budget.empty());
}

TEST(TopicRateCaps, ShedConnectionKeepsItsSendState)
{
  // Nothing was sent, so nothing about the connection's send state may
  // move: stamping last_sent_time would additionally delay the topic
  // under its `period` for a message it never received.
  std::map<std::string, RemoteDetails> table;
  table["operator"] = remote("/bulk", "vpn", 1000, 0.5f);

  ASSERT_EQ(selectRateLimitedConnections(table, kT0, 5000).due.count("operator"), 1u);
  const auto after_send =
    table["operator"].connection_rates["vpn"].last_sent_time;
  ASSERT_EQ(after_send.nanoseconds(), kT0.nanoseconds());

  // Well past the period, but still over the byte cap.
  auto shed = selectRateLimitedConnections(table, at(1.0), 5000);
  ASSERT_TRUE(shed.due.empty());
  EXPECT_EQ(table["operator"].connection_rates["vpn"].last_sent_time.nanoseconds(),
            after_send.nanoseconds())
    << "a shed connection's last_sent_time must not move";
}

TEST(TopicRateCaps, PeriodGateRunsFirst)
{
  // A connection held back by `period` must not be charged: it was never
  // going to receive this message, and charging it would make the byte
  // cap shed traffic the period rule already shaped away.
  std::map<std::string, RemoteDetails> table;
  table["operator"] = remote("/bulk", "vpn", 1000, 10.0f);

  ASSERT_EQ(selectRateLimitedConnections(table, kT0, 900).due.count("operator"), 1u);
  const double charged_once =
    table["operator"].connection_rates["vpn"].budget_used_bytes;
  ASSERT_GT(charged_once, 0.0);

  // Not due for another 10 s. Several offers in the meantime must leave
  // the budget alone (bar the drain that time itself causes).
  for(int i = 1; i < 5; ++i)
    EXPECT_TRUE(selectRateLimitedConnections(table, at(i * 0.1), 900).due.empty());
  EXPECT_LT(table["operator"].connection_rates["vpn"].budget_used_bytes,
            charged_once + 1.0)
    << "an offer the period rule refused must not be charged to the cap";
}

TEST(TopicRateCaps, CapAppliesToRelayedTrafficIdentically)
{
  // Same table, same cap, reached through the relay path's exclude_remote
  // argument. It is the same code and the same state, which is the point
  // of enforcing here rather than in either sender.
  std::map<std::string, RemoteDetails> local_table;
  local_table["operator"] = remote("/bulk", "vpn", 1000);
  std::map<std::string, RemoteDetails> relay_table;
  relay_table["boat"] = remote("/bulk", "vpn", 1000);
  relay_table["operator"] = remote("/bulk", "vpn", 1000);

  ASSERT_EQ(selectRateLimitedConnections(local_table, kT0, 5000).due.count("operator"), 1u);
  ASSERT_EQ(selectRateLimitedConnections(relay_table, kT0, 5000, "boat").due.count("operator"), 1u);

  EXPECT_TRUE(selectRateLimitedConnections(local_table, kT0, 5000).due.empty());
  EXPECT_TRUE(selectRateLimitedConnections(relay_table, kT0, 5000, "boat").due.empty())
    << "relayed traffic must be capped by exactly the same rule";
}

// ---------------------------------------------------------------------
// Accounting: a shed message must be visible.
// ---------------------------------------------------------------------

TEST(TopicRateCaps, DropRecordLandsInTopicStatistics)
{
  udp_bridge::SelectedConnections over_budget;
  over_budget["operator"].push_back("vpn");

  MessageStatistics statistics;
  // Two samples so MessageStatistics::get() has a time span to divide by.
  statistics.add(overBudgetDropRecord(over_budget, 5000, kT0));
  statistics.add(overBudgetDropRecord(over_budget, 5000, at(1.0)));

  auto rows = statistics.get();
  bool found = false;
  for(const auto& row: rows)
  {
    if(row.destination_node != "operator" || row.connection_id != "vpn")
      continue;
    found = true;
    EXPECT_GT(row.send.dropped_bytes_per_second, 0.0f)
      << "a per-topic drop must show up in the topic's send DataRates, the "
         "way a connection-level drop does";
    EXPECT_FLOAT_EQ(row.send.success_bytes_per_second, 0.0f);
    EXPECT_FLOAT_EQ(row.send.failed_bytes_per_second, 0.0f);
  }
  EXPECT_TRUE(found) << "no TopicStatistics row for the shed destination";
}

TEST(TopicRateCaps, DropRecordMarksEveryShedDestination)
{
  udp_bridge::SelectedConnections over_budget;
  over_budget["operator"].push_back("vpn");
  over_budget["operator"].push_back("cell");
  over_budget["hub"].push_back("wan");

  auto record = overBudgetDropRecord(over_budget, 1234, kT0);
  EXPECT_EQ(record.message_size, 1234);
  EXPECT_EQ(record.sent_size, 1234);
  EXPECT_EQ(record.send_results["operator"]["vpn"], SendResult::dropped);
  EXPECT_EQ(record.send_results["operator"]["cell"], SendResult::dropped);
  EXPECT_EQ(record.send_results["hub"]["wan"], SendResult::dropped);
}

// ---------------------------------------------------------------------
// Configuration and runtime tuning, against a real node.
// ---------------------------------------------------------------------

namespace
{

const std::string kRemote = "robot_a";
const std::string kConnection = "vpn";
// The topics_list label spells a parameter path segment; the resolved
// `source` is what the routing table is keyed by. Kept different on
// purpose, so a mapping that used the label would fail these tests.
const std::string kTopicLabel = "bulk";
const std::string kSourceTopic = "/bulk";

std::string topicParam(const std::string& key)
{
  return "remotes." + kRemote + ".connections." + kConnection + ".topics."
       + kTopicLabel + "." + key;
}

// Same shape as test_runtime_tunables.cpp's helper: `port` 0 so the bind
// cannot collide with a running bridge, loopback destination, nothing
// ever sent.
class Bridge
{
public:
  Bridge(const std::string& node_name, int64_t topic_cap)
  : node_(std::make_shared<udp_bridge::UDPBridge>(node_name))
  {
    const std::string connection_base =
      "remotes." + kRemote + ".connections." + kConnection + ".";
    node_->declare_parameter("port", 0);
    node_->declare_parameter("name", std::string("test_bridge"));
    node_->declare_parameter("remotes_list", std::vector<std::string>{kRemote});
    node_->declare_parameter("remotes." + kRemote + ".connections_list",
                             std::vector<std::string>{kConnection});
    node_->declare_parameter(connection_base + "host", std::string("127.0.0.1"));
    node_->declare_parameter(connection_base + "port", 45999);
    node_->declare_parameter(connection_base + "topics_list",
                             std::vector<std::string>{kTopicLabel});
    node_->declare_parameter(topicParam("source"), kSourceTopic);
    // A negative argument means "leave the key out entirely", so the
    // absent-key path is tested as an absent key and not as an explicit 0.
    if(topic_cap >= 0)
      node_->declare_parameter(topicParam("maximum_bytes_per_second"), topic_cap);
  }

  bool configure()
  {
    return node_->configure().id() ==
           lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
  }

  uint32_t cap()
  {
    ConnectionRateInfo info;
    if(!node_->topicRateInfoForTest(kSourceTopic, kRemote, kConnection, info))
      return std::numeric_limits<uint32_t>::max();  // distinctive "no entry"
    return info.maximum_bytes_per_second;
  }

  udp_bridge::UDPBridge& node() { return *node_; }

private:
  std::shared_ptr<udp_bridge::UDPBridge> node_;
};

}  // namespace

TEST(TopicRateCapConfig, ConfiguredCapReachesTheRoutingTable)
{
  Bridge bridge("topic_cap_config", 200000);
  ASSERT_TRUE(bridge.configure());
  EXPECT_EQ(bridge.cap(), 200000u)
    << "the configured cap must reach the routing table the senders meter "
       "against, keyed by the RESOLVED source topic";
}

TEST(TopicRateCapConfig, AbsentCapIsUnlimited)
{
  Bridge bridge("topic_cap_absent", -1);  // key not declared at all
  ASSERT_TRUE(bridge.configure());
  EXPECT_EQ(bridge.cap(), 0u)
    << "an absent maximum_bytes_per_second means unlimited, so every "
       "pre-existing config behaves exactly as it did";
}

TEST(TopicRateCapConfig, RuntimeSetChangesTheCap)
{
  Bridge bridge("topic_cap_runtime", 200000);
  ASSERT_TRUE(bridge.configure());
  ASSERT_EQ(bridge.cap(), 200000u);

  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(topicParam("maximum_bytes_per_second"), int64_t{50000}));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_EQ(bridge.cap(), 50000u)
    << "tuning a per-topic cap is exactly the thing an operator does mid-run";
}

TEST(TopicRateCapConfig, RuntimeZeroRemovesTheCap)
{
  Bridge bridge("topic_cap_runtime_zero", 200000);
  ASSERT_TRUE(bridge.configure());

  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(topicParam("maximum_bytes_per_second"), int64_t{0}));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_EQ(bridge.cap(), 0u)
    << "0 through the parameter unambiguously means unlimited — the "
       "\"zero retains\" rule belongs to addSubscriberConnection, whose "
       "service callers carry no cap field at all";
}

TEST(TopicRateCapConfig, RuntimeNegativeRejected)
{
  Bridge bridge("topic_cap_runtime_negative", 200000);
  ASSERT_TRUE(bridge.configure());

  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(topicParam("maximum_bytes_per_second"), int64_t{-5}));
  EXPECT_FALSE(result.successful);
  EXPECT_NE(result.reason.find("maximum_bytes_per_second"), std::string::npos)
    << "got: " << result.reason;
  EXPECT_EQ(bridge.cap(), 200000u) << "a refused set must change nothing";
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  rclcpp::init(0, nullptr);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

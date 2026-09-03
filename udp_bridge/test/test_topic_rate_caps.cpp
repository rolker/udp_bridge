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
#include <optional>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lifecycle_msgs/msg/state.hpp"
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/destination_selection.h"
#include "udp_bridge/packet.h"
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

// The exact condition relayToOtherRemotes branches on to pick between
// RelayDropReason::NoDestinationDue and RelayDropReason::TopicRateLimited
// (`over_budget.empty() ? ... : ...`). The two are counted apart because
// they are tuned in different places, so the selector has to leave them
// distinguishable: "nothing due" and "everything due was over budget"
// must not both arrive as an empty selection with nothing else set.
TEST(TopicRateCaps, ShedEverythingIsDistinguishableFromNothingDue)
{
  // Nothing due: a long `period` that has not elapsed, no cap at all.
  std::map<std::string, RemoteDetails> not_due;
  not_due["operator"] = remote("/bulk", "vpn", 0, 10.0f);
  ASSERT_EQ(selectRateLimitedConnections(not_due, kT0, 100).due.count("operator"), 1u);
  auto held = selectRateLimitedConnections(not_due, at(0.1), 100);
  EXPECT_TRUE(held.due.empty());
  EXPECT_TRUE(held.over_budget.empty())
    << "the `period` rule holding a message back is NoDestinationDue — "
       "nothing was over budget, so nothing may be reported as such";

  // Everything due, everything over its cap.
  std::map<std::string, RemoteDetails> capped;
  capped["operator"] = remote("/bulk", "vpn", 1000);
  ASSERT_EQ(selectRateLimitedConnections(capped, kT0, 5000).due.count("operator"), 1u);
  auto shed = selectRateLimitedConnections(capped, kT0, 5000);
  EXPECT_TRUE(shed.due.empty());
  EXPECT_FALSE(shed.over_budget.empty())
    << "an item whose every due destination was shed by the per-topic cap "
       "is TopicRateLimited, and the relay sink can only tell from this";
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

std::string topicParam(const std::string& key,
                       const std::string& connection = kConnection)
{
  return "remotes." + kRemote + ".connections." + connection + ".topics."
       + kTopicLabel + "." + key;
}

// Same shape as test_runtime_tunables.cpp's helper: `port` 0 so the bind
// cannot collide with a running bridge, loopback destination, nothing
// ever sent.
class Bridge
{
public:
  // `connection` is the CONFIGURED connection id, i.e. what the
  // parameter path is spelled with. It is deliberately settable so the
  // over-long-id case (the repo's own example_params.yaml carries
  // "telemetry", nine characters) can be exercised.
  Bridge(const std::string& node_name, std::optional<int64_t> topic_cap,
         const std::string& connection = kConnection)
  : node_(std::make_shared<udp_bridge::UDPBridge>(node_name)),
    connection_(connection)
  {
    const std::string connection_base =
      "remotes." + kRemote + ".connections." + connection_ + ".";
    node_->declare_parameter("port", 0);
    node_->declare_parameter("name", std::string("test_bridge"));
    node_->declare_parameter("remotes_list", std::vector<std::string>{kRemote});
    node_->declare_parameter("remotes." + kRemote + ".connections_list",
                             std::vector<std::string>{connection_});
    node_->declare_parameter(connection_base + "host", std::string("127.0.0.1"));
    node_->declare_parameter(connection_base + "port", 45999);
    node_->declare_parameter(connection_base + "topics_list",
                             std::vector<std::string>{kTopicLabel});
    node_->declare_parameter(topicParam("source", connection_), kSourceTopic);
    // std::nullopt means "leave the key out entirely", so the absent-key
    // path is tested as an absent key and not as an explicit 0 — and a
    // NEGATIVE value can still be declared, which the configure-side
    // clamp tests need.
    if(topic_cap.has_value())
      node_->declare_parameter(
        topicParam("maximum_bytes_per_second", connection_), *topic_cap);
  }

  bool configure()
  {
    return node_->configure().id() ==
           lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
  }

  // Back to UNCONFIGURED. Re-configuring afterwards is safe only because
  // the fixture binds `port` 0 — nothing closes the socket (issue #66),
  // so a fixed port would hit EADDRINUSE and exit(1) on the second bind.
  bool cleanup()
  {
    return node_->cleanup().id() ==
           lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED;
  }

  // Looked up by whatever id the caller names, defaulting to the
  // configured one. `capUnderId` is what the canonicalization test needs:
  // it asserts WHICH key the routing table is built under.
  uint32_t cap() { return capUnderId(connection_); }

  uint32_t capUnderId(const std::string& connection_id)
  {
    ConnectionRateInfo info;
    if(!node_->topicRateInfoForTest(kSourceTopic, kRemote, connection_id, info))
      return std::numeric_limits<uint32_t>::max();  // distinctive "no entry"
    return info.maximum_bytes_per_second;
  }

  udp_bridge::UDPBridge& node() { return *node_; }
  const std::string& connection() const { return connection_; }

private:
  std::shared_ptr<udp_bridge::UDPBridge> node_;
  std::string connection_;
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
  Bridge bridge("topic_cap_absent", std::nullopt);  // key not declared at all
  ASSERT_TRUE(bridge.configure());
  EXPECT_EQ(bridge.cap(), 0u)
    << "an absent maximum_bytes_per_second means unlimited, so every "
       "pre-existing config behaves exactly as it did";
}

// The configure path CLAMPS what the runtime path refuses, because a
// params file has no return channel (connection_tunables.h). Only the
// runtime half of that asymmetry was covered; the asymmetry is this
// branch's central validation policy, so both halves need pinning.
TEST(TopicRateCapConfig, ConfiguredNegativeCapWarnsAndBecomesNoCap)
{
  Bridge bridge("topic_cap_config_negative", -5);
  ASSERT_TRUE(bridge.configure())
    << "a nonsensical scalar must not stop a telemetry bridge starting";
  EXPECT_EQ(bridge.cap(), 0u)
    << "a negative cap degrades to no cap — today's behaviour and the "
       "least surprising reading — where a runtime set is refused";
}

TEST(TopicRateCapConfig, ConfiguredOverRangeCapIsClampedNotWrapped)
{
  // Past uint32. Cast unchecked it would wrap to a small cap and shed
  // almost everything on a config asking for the opposite.
  constexpr int64_t kOverRange =
    static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1000;
  Bridge bridge("topic_cap_config_over_range", kOverRange);
  ASSERT_TRUE(bridge.configure());
  EXPECT_EQ(bridge.cap(), std::numeric_limits<uint32_t>::max())
    << "clamped to the largest representable cap, not wrapped";
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

// A connection id longer than the on-wire limit must key the routing
// table by its CANONICAL (truncated) form — the same form connections_
// is keyed by and the same form send() records every send_result under.
//
// Keyed raw, the shedding and the sending of one topic on one link land
// in two different TopicStatistics rows (the drop record under
// "telemetry", the successful send under "telemet"), so neither row
// shows what the link actually did — and two configured ids agreeing in
// their first `maximum_connection_id_size - 1` characters become ONE
// Connection with TWO independent per-topic budgets, carrying up to
// twice the configured cap. The repo's own example_params.yaml uses
// "telemetry", so this is not a hypothetical id.
TEST(TopicRateCapConfig, OverLongConnectionIdIsKeyedCanonically)
{
  const std::string configured = "telemetry";
  const std::string canonical = udp_bridge::truncate_connection_id(configured);
  ASSERT_NE(configured, canonical)
    << "the fixture only tests anything if the id really is over-long";

  Bridge bridge("topic_cap_long_id", 200000, configured);
  ASSERT_TRUE(bridge.configure());

  EXPECT_EQ(bridge.capUnderId(canonical), 200000u)
    << "the routing table must be keyed by the canonical id, which is "
       "what Connection::id() — and so every send_results record — uses";
  EXPECT_EQ(bridge.capUnderId(configured),
            std::numeric_limits<uint32_t>::max())
    << "and NOT by the raw configured id: a second entry under the raw "
       "name is a second budget for the same link";

  // The live Connection and the routing entry must agree on the key, or
  // the per-topic cap meters a link the statistics report under another
  // name.
  auto connection = bridge.node().connectionForTest(kRemote, configured);
  ASSERT_TRUE(connection != nullptr);
  EXPECT_EQ(connection->id(), canonical);
}

// The runtime path has to resolve the same way: the parameter PATH is
// spelled with the configured id (that is what the operator wrote), but
// the target it resolves to is the canonical key.
TEST(TopicRateCapConfig, RuntimeSetResolvesAnOverLongConnectionId)
{
  const std::string configured = "telemetry";
  const std::string canonical = udp_bridge::truncate_connection_id(configured);

  Bridge bridge("topic_cap_long_id_runtime", 200000, configured);
  ASSERT_TRUE(bridge.configure());

  auto result = bridge.node().set_parameter(rclcpp::Parameter(
    topicParam("maximum_bytes_per_second", configured), int64_t{50000}));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_EQ(bridge.capUnderId(canonical), 50000u)
    << "a set spelled with the configured id must reach the entry the "
       "senders meter against";
}

// A cap must be REMOVABLE by the config file that added it. `on_configure`
// passes the value it read, engaged, so 0 there means "no cap" — the
// documented meaning of the key. Under the older "0 retains" rule a cap
// could be added by a params file and never taken back out by one: the
// key set to 0 read back as 0 through `ros2 param get` while the selector
// went on shedding at the previous value. That is the accepted-but-inert
// shape this whole branch exists to remove, and it is worse here than
// elsewhere because the operator's evidence says the cap is gone.
TEST(TopicRateCapConfig, ReconfigureCanRemoveACap)
{
  Bridge bridge("topic_cap_reconfigure_zero", 200000);
  ASSERT_TRUE(bridge.configure());
  ASSERT_EQ(bridge.cap(), 200000u);

  ASSERT_TRUE(bridge.cleanup());
  // With the callback unregistered (on_cleanup drops it), this writes the
  // parameter store only — exactly what editing the YAML and restarting
  // the lifecycle does.
  ASSERT_TRUE(bridge.node().set_parameter(
    rclcpp::Parameter(topicParam("maximum_bytes_per_second"),
                      int64_t{0})).successful);
  ASSERT_TRUE(bridge.configure());

  EXPECT_EQ(bridge.cap(), 0u)
    << "a re-configure with the key set back to 0 must actually remove "
       "the cap, not read back as 0 while the old value keeps shedding";
}

// The other half of the same rule: the wire/service callers carry no cap
// field at all (adding one to RemoteSubscribeInternal would change the
// message's type hash and force a coordinated redeploy of both ends), so
// they must RETAIN whatever is configured. A peer re-subscribing must not
// be able to wipe a cap the operator put in place — which is what makes
// "an engaged value is authoritative" safe to add above.
TEST(TopicRateCapConfig, AReadvertiseDoesNotWipeTheCap)
{
  Bridge bridge("topic_cap_readvertise", 200000);
  ASSERT_TRUE(bridge.configure());
  ASSERT_EQ(bridge.cap(), 200000u);

  bridge.node().readvertiseForTest(kSourceTopic, kSourceTopic, 10, 0.0f,
                                   kRemote, kConnection);
  EXPECT_EQ(bridge.cap(), 200000u)
    << "a re-advertise carries no cap field, so it must leave the cap alone";
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

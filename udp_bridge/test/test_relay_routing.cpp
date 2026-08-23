// Routing tests for remote->remote relay (issue #51).
//
// The bridge relays a message it received from one remote to the OTHER
// remotes whose per-remote topics_list carries the same topic. There is no
// relay parameter: the topic lists ARE the routing table (ROS 1 semantics),
// so everything worth testing about relay is a property of the selection
// step — which destinations are chosen for a message that arrived from a
// given sender, and how their rate limits are applied.
//
// That selection is destination_selection.h, shared verbatim by both
// senders: UDPBridge::callback() (locally published message, no sender to
// exclude) and UDPBridge::relayToOtherRemotes() (received message, sender
// excluded). Testing it here therefore tests the real routing decision, with
// no node, no socket and no DDS — the same isolation-by-extraction pattern
// as test_stale_packet_gate / test_reorder_buffer / test_giveup_diagnostic.
//
// Pinned contract points:
//
//   ThreeNodeRelayReachesTheOtherRemote — A -> hub -> B: a hub carrying the
//                                 topic for both remotes forwards A's
//                                 message to B (and only B). The hub's own
//                                 local publication of that message is
//                                 decodeData's unchanged publish_queue_
//                                 push, which relay adds to rather than
//                                 branches around — the probe below is what
//                                 decides whether relay happens at all, and
//                                 it says yes here while the local path is
//                                 untouched.
//   EchoIsNeverSentBackToTheSender — the loop rule. A symmetric two-host
//                                 pair must relay nothing; this test fails
//                                 if the exclude_remote filter is removed
//                                 from EITHER the probe or the selection.
//   RoutingTableFidelity        — a remote whose topics_list omits the topic
//                                 receives no relayed traffic for it, while
//                                 a sibling that lists it does.
//   RelayRateLimitMatchesLocalOrigin — a `period` on a relay destination
//                                 throttles identically to the same period
//                                 on a local-origin destination, because it
//                                 is the same code and the same
//                                 last_sent_time state.
//   ExcludedRemoteRateStateUntouched — excluding the sender must not stamp
//                                 its last_sent_time; otherwise a relay
//                                 would silently delay that remote's next
//                                 locally-originated message.
//   NegativeAndZeroPeriodsPreserved / SamePeriodConnectionsSendAsAGroup —
//                                 the extraction preserved callback()'s
//                                 exact pre-#51 gating behaviour.

#include "udp_bridge/destination_selection.h"

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using udp_bridge::ConnectionRateInfo;
using udp_bridge::RemoteDetails;
using udp_bridge::SelectedConnections;
using udp_bridge::hasRelayDestination;
using udp_bridge::selectRateLimitedConnections;

namespace
{

const rclcpp::Time kT0(1000, 0, RCL_ROS_TIME);

// One remote listing `topic` (as its destination topic) over one connection
// with the given period. period 0 = no rate limit, which is the default in
// every config in this repo.
RemoteDetails remote(const std::string& destination_topic,
                     const std::string& connection_id,
                     float period = 0.0f)
{
  RemoteDetails details;
  details.destination_topic = destination_topic;
  ConnectionRateInfo rate;
  rate.period = period;
  rate.last_sent_time = rclcpp::Time(0, 0, RCL_ROS_TIME);
  details.connection_rates[connection_id] = rate;
  return details;
}

// Names of the remotes selected, for readable assertions.
std::vector<std::string> selectedRemotes(const SelectedConnections& selected)
{
  std::vector<std::string> names;
  for(const auto& entry: selected)
    names.push_back(entry.first);
  return names;
}

} // namespace

// A -> hub -> B. The hub's routing table for /status lists both the boat it
// heard from and the operator that also asked for /status; the message is
// forwarded to the operator, and to nobody else.
TEST(RelayRouting, ThreeNodeRelayReachesTheOtherRemote)
{
  std::map<std::string, RemoteDetails> table;
  table["boat"] = remote("/status", "wifi");
  table["operator"] = remote("/status", "wifi");

  // The probe the socket-drain thread runs: yes, there is somewhere to
  // relay, so decodeData pays for the copy. The local publish path it also
  // takes is unconditional and unchanged.
  EXPECT_TRUE(hasRelayDestination(table, "boat"));

  auto selected = selectRateLimitedConnections(table, kT0, "boat");
  EXPECT_EQ(selectedRemotes(selected), (std::vector<std::string>{"operator"}));
  ASSERT_EQ(selected.count("operator"), 1u);
  EXPECT_EQ(selected["operator"], (std::vector<std::string>{"wifi"}));
  EXPECT_EQ(selected.count("boat"), 0u);
}

// The loop rule, and the reason no relay opt-in flag is needed: a symmetric
// pair (the shape of every existing deployment) relays nothing at all. If the
// exclude_remote filter is dropped from either the probe or the selection,
// this test fails — a message from A would be sent straight back to A.
TEST(RelayRouting, EchoIsNeverSentBackToTheSender)
{
  std::map<std::string, RemoteDetails> table;
  table["operator"] = remote("/status", "wifi");

  EXPECT_FALSE(hasRelayDestination(table, "operator"));

  auto selected = selectRateLimitedConnections(table, kT0, "operator");
  EXPECT_TRUE(selected.empty());
}

// A packet that never passed through unwrap() carries no source_node, so the
// loop rule has nothing to compare against and would exclude nobody. Such a
// packet must not be relayed at all: on an unauthenticated transport (#53) a
// bare injected Data packet would otherwise be fanned out to every remote
// listing the topic. Fails if the empty-sender guard is removed from the
// probe.
TEST(RelayRouting, UnnamedSenderIsNeverRelayed)
{
  std::map<std::string, RemoteDetails> table;
  table["boat"] = remote("/status", "wifi");
  table["operator"] = remote("/status", "wifi");

  EXPECT_FALSE(hasRelayDestination(table, ""))
    << "a packet with no source_node has no loop rule and must not be relayed";

  // The rate-limit state of every remote is likewise untouched: the probe
  // said no, so no selection call is made for such a packet.
  EXPECT_EQ(table["boat"].connection_rates["wifi"].last_sent_time.nanoseconds(), 0);
  EXPECT_EQ(table["operator"].connection_rates["wifi"].last_sent_time.nanoseconds(), 0);
}

// Relay follows the routing table and nothing else: a remote is reached only
// for the topics its own topics_list carries. Modelled the way the bridge
// stores it — subscribers_ is keyed by source topic, and a remote that did
// not ask for a topic simply has no entry in that topic's table.
TEST(RelayRouting, RoutingTableFidelity)
{
  // /status: both operators asked for it.
  std::map<std::string, RemoteDetails> status_table;
  status_table["boat"] = remote("/status", "wifi");
  status_table["operator_a"] = remote("/status", "wifi");
  status_table["operator_b"] = remote("/status", "wifi");

  // /sonar: only operator_a asked for it.
  std::map<std::string, RemoteDetails> sonar_table;
  sonar_table["boat"] = remote("/sonar", "wifi");
  sonar_table["operator_a"] = remote("/sonar", "wifi");

  auto status_selected = selectRateLimitedConnections(status_table, kT0, "boat");
  EXPECT_EQ(selectedRemotes(status_selected),
            (std::vector<std::string>{"operator_a", "operator_b"}));

  auto sonar_selected = selectRateLimitedConnections(sonar_table, kT0, "boat");
  EXPECT_EQ(selectedRemotes(sonar_selected),
            (std::vector<std::string>{"operator_a"}));
  EXPECT_EQ(sonar_selected.count("operator_b"), 0u);
}

// Rate-limit parity: a relay destination with period P is gated exactly as a
// local-origin destination with period P, because both go through this one
// function against the same per-connection last_sent_time.
TEST(RelayRouting, RelayRateLimitMatchesLocalOrigin)
{
  const float period = 1.0f;

  // Local-origin side: one remote, no sender to exclude.
  std::map<std::string, RemoteDetails> local_table;
  local_table["operator"] = remote("/status", "wifi", period);

  // Relay side: same destination, reached by forwarding from "boat".
  std::map<std::string, RemoteDetails> relay_table;
  relay_table["boat"] = remote("/status", "wifi", period);
  relay_table["operator"] = remote("/status", "wifi", period);

  // First message: both send (last_sent_time is the never-sent sentinel).
  EXPECT_EQ(selectRateLimitedConnections(local_table, kT0).count("operator"), 1u);
  EXPECT_EQ(selectRateLimitedConnections(relay_table, kT0, "boat").count("operator"), 1u);

  // Half a period later: both withhold.
  const rclcpp::Time half = kT0 + rclcpp::Duration::from_seconds(0.5);
  EXPECT_EQ(selectRateLimitedConnections(local_table, half).count("operator"), 0u);
  EXPECT_EQ(selectRateLimitedConnections(relay_table, half, "boat").count("operator"), 0u);

  // Past the period: both send again.
  const rclcpp::Time past = kT0 + rclcpp::Duration::from_seconds(1.5);
  EXPECT_EQ(selectRateLimitedConnections(local_table, past).count("operator"), 1u);
  EXPECT_EQ(selectRateLimitedConnections(relay_table, past, "boat").count("operator"), 1u);
}

// Excluding the sender must be side-effect free. If the exclusion were
// applied after the last_sent_time stamp (or by filtering the result), a
// relay would consume the sender's rate-limit budget and delay the next
// message the bridge sends it on its own account.
TEST(RelayRouting, ExcludedRemoteRateStateUntouched)
{
  std::map<std::string, RemoteDetails> table;
  table["boat"] = remote("/status", "wifi", 1.0f);
  table["operator"] = remote("/status", "wifi", 1.0f);

  selectRateLimitedConnections(table, kT0, "boat");

  EXPECT_EQ(table["boat"].connection_rates["wifi"].last_sent_time.nanoseconds(), 0)
    << "the excluded sender's rate state must not be stamped by a relay";
  EXPECT_EQ(table["operator"].connection_rates["wifi"].last_sent_time, kT0);
}

// The extraction from callback() preserved its exact gating: a negative
// period means never send, zero means no limit.
TEST(RelayRouting, NegativeAndZeroPeriodsPreserved)
{
  std::map<std::string, RemoteDetails> table;
  table["never"] = remote("/status", "wifi", -1.0f);
  table["always"] = remote("/status", "wifi", 0.0f);

  auto first = selectRateLimitedConnections(table, kT0);
  EXPECT_EQ(selectedRemotes(first), (std::vector<std::string>{"always"}));

  // Immediately again: period 0 is unthrottled, negative is still silent.
  auto second = selectRateLimitedConnections(table, kT0);
  EXPECT_EQ(selectedRemotes(second), (std::vector<std::string>{"always"}));
}

// Also preserved: connections that share a period are sent as a group. Once
// one connection with period P is due, every other connection with period P
// goes out with it rather than skewing to its own schedule.
TEST(RelayRouting, SamePeriodConnectionsSendAsAGroup)
{
  std::map<std::string, RemoteDetails> table;
  RemoteDetails details;
  details.destination_topic = "/status";
  // "a" is due (never sent); "b" shares its period but was sent just now.
  ConnectionRateInfo due;
  due.period = 1.0f;
  due.last_sent_time = rclcpp::Time(0, 0, RCL_ROS_TIME);
  ConnectionRateInfo recent;
  recent.period = 1.0f;
  recent.last_sent_time = kT0;
  details.connection_rates["a"] = due;
  details.connection_rates["b"] = recent;
  table["operator"] = details;

  auto selected = selectRateLimitedConnections(table, kT0);
  ASSERT_EQ(selected.count("operator"), 1u);
  EXPECT_EQ(selected["operator"], (std::vector<std::string>{"a", "b"}));
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

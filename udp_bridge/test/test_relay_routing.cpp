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
//   ConfiguredNameIsTheIdentityNotTheLabel / DuplicateRemoteIdentityIsRejected
//                               / LabelDifferingFromWireNameStillNeverEchoes —
//                                 the identity namespace the loop rule
//                                 compares in. A static config keys the
//                                 routing table by the remote's WIRE name
//                                 (`remotes.<label>.name`, else the label),
//                                 because that is what arrives in
//                                 WrappedPacket::source_node; keying by the
//                                 label made the hub echo a remote's own
//                                 traffic back to it even with one remote
//                                 configured.
//   OverLongRemoteIdentityIsRejected / MaximumLengthIdentityStillClosesTheLoopRule
//                               / UnterminatedWireNameIsReadWithinItsField —
//                                 the identity must be REPRESENTABLE, or the
//                                 compare above can never succeed. An
//                                 over-long name is refused rather than
//                                 truncated (truncation is what manufactures
//                                 the mismatch), a name at the limit round
//                                 trips whole, and a wire field with no null
//                                 terminator is read within its bounds.
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
#include "udp_bridge/relay_item.h"
#include "udp_bridge/packet.h"
#include "udp_bridge/remote_identity.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using udp_bridge::ConnectionRateInfo;
using udp_bridge::DestinationConfig;
using udp_bridge::RemoteDetails;
using udp_bridge::SelectedConnections;
using udp_bridge::applyRelayDestination;
using udp_bridge::hasRelayDestination;
using udp_bridge::resolveRemoteIdentities;
using udp_bridge::resolveRemoteIdentity;
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

// The loop rule compares the wire `source_node` against the keys of the
// topic's routing table, so a static config MUST key that table by the name
// the remote calls itself on the wire -- not by its `remotes_list` label.
//
// Before issue #51's identity fix, on_configure keyed everything by the
// label and never read `remotes.<label>.name`, so a remote whose own name
// differed from the hub's label for it matched nothing in the loop rule and
// the hub relayed that remote's traffic straight back to it. That happens
// with a SINGLE remote configured -- the configuration relay is supposed to
// leave inert -- which is why it is a correctness bug and not a
// documentation gap.
//
// These tests model the two halves. `resolveRemoteIdentity` is the single
// place a static config decides the key (on_configure calls it via
// resolveRemoteIdentities); keying the table through it and then handing the
// selector the wire name is exactly the production path, and the echo below
// is what the pre-fix keying produced.
TEST(RelayRouting, ConfiguredNameIsTheIdentityNotTheLabel)
{
  // Unset `name` keeps the historical behaviour: the label is the identity.
  EXPECT_EQ(resolveRemoteIdentity("robot_a", ""), "robot_a");
  // Set `name` wins -- it is what arrives in WrappedPacket::source_node.
  EXPECT_EQ(resolveRemoteIdentity("robot_a", "robot_a_bridge"), "robot_a_bridge");

  std::string error;
  auto identities = resolveRemoteIdentities({{"robot_a", "robot_a_bridge"},
                                             {"operator_1", ""}}, &error);
  EXPECT_TRUE(error.empty());
  ASSERT_EQ(identities.size(), 2u);
  EXPECT_EQ(identities.at("robot_a"), "robot_a_bridge");
  EXPECT_EQ(identities.at("operator_1"), "operator_1");
}

// Two labels resolving to one wire name would collide in remote_nodes_ and
// in every topic's routing table, the second silently overwriting the
// first's connections. on_configure fails the transition on this.
TEST(RelayRouting, DuplicateRemoteIdentityIsRejected)
{
  std::string error;
  auto identities = resolveRemoteIdentities({{"robot_a", "hub"},
                                             {"robot_b", "hub"}}, &error);
  EXPECT_TRUE(identities.empty());
  EXPECT_FALSE(error.empty()) << "a duplicate identity must be reported, not merged";
  EXPECT_NE(error.find("hub"), std::string::npos);
}

// A label repeated in `remotes_list` names one parameter block, so it is
// the same remote twice, not two remotes colliding. Before #51 the repeat
// was simply configured twice, idempotently; failing the transition on it
// (with "remotes 'a' and 'a' both resolve to...") would be a regression
// against a config that used to work.
TEST(RelayRouting, RepeatedLabelIsIdempotentNotACollision)
{
  std::string error;
  auto identities = resolveRemoteIdentities({{"robot_a", "robot_a_bridge"},
                                             {"robot_a", "robot_a_bridge"},
                                             {"operator_1", ""}}, &error);
  EXPECT_TRUE(error.empty()) << "a remote cannot collide with itself";
  ASSERT_EQ(identities.size(), 2u);
  EXPECT_EQ(identities.at("robot_a"), "robot_a_bridge");
  EXPECT_EQ(identities.at("operator_1"), "operator_1");

  // A genuine collision is still rejected when the repeat is present.
  error.clear();
  auto colliding = resolveRemoteIdentities({{"robot_a", "hub"},
                                            {"robot_a", "hub"},
                                            {"robot_b", "hub"}}, &error);
  EXPECT_TRUE(colliding.empty());
  EXPECT_FALSE(error.empty());
}

// The regression: one remote, configured under a label that differs from the
// name it uses on the wire. Keyed as the fixed on_configure keys it, the loop
// rule matches and nothing is relayed. Keyed by the label -- the pre-fix
// behaviour, asserted below for contrast -- the hub echoes the sender's own
// message back to it.
TEST(RelayRouting, LabelDifferingFromWireNameStillNeverEchoes)
{
  const std::string label = "robot_a";
  const std::string wire_name = "robot_a_bridge";  // remotes.robot_a.name

  std::string error;
  auto identities = resolveRemoteIdentities({{label, wire_name}}, &error);
  ASSERT_TRUE(error.empty());

  // addSubscriberConnection is called with this key by on_configure.
  std::map<std::string, RemoteDetails> table;
  table[identities.at(label)] = remote("/status", "wifi");

  // The packet arrives carrying the sender's own name.
  EXPECT_FALSE(hasRelayDestination(table, wire_name))
    << "a single-remote config must have nowhere to relay, whatever the label is";
  auto selected = selectRateLimitedConnections(table, kT0, wire_name);
  EXPECT_TRUE(selected.empty()) << "the hub relayed the sender's message back to it";
  EXPECT_EQ(table[wire_name].connection_rates["wifi"].last_sent_time.nanoseconds(), 0)
    << "an excluded sender's rate-limit state must be untouched";

  // What the label-keyed table did, so the failure this pins is explicit.
  std::map<std::string, RemoteDetails> label_keyed;
  label_keyed[label] = remote("/status", "wifi");
  EXPECT_TRUE(hasRelayDestination(label_keyed, wire_name))
    << "keying by the label is what made the loop rule miss";
}

// The wire field is `char source_node[24]`, so 23 characters is the longest
// name that survives a round trip. An identity longer than that cannot be
// compared successfully against what actually arrives, so the loop rule
// would be permanently inert for that remote and the hub would echo its
// traffic back to it. Truncating to fit is what manufactures that mismatch
// -- and would let two identities differing only after char 23 collapse
// onto one wire name, reopening the collision the function above rejects.
// So it is refused, with a message an operator can act on.
TEST(RelayRouting, OverLongRemoteIdentityIsRejected)
{
  const std::string too_long(udp_bridge::maximum_node_name_length + 1, 'x');

  std::string error;
  auto identities = resolveRemoteIdentities({{"robot_a", too_long}}, &error);
  EXPECT_TRUE(identities.empty())
    << "an unrepresentable identity must fail on_configure, not be truncated";
  ASSERT_FALSE(error.empty());
  EXPECT_NE(error.find(too_long), std::string::npos)
    << "the message must quote the offending name";
  EXPECT_NE(error.find(std::to_string(udp_bridge::maximum_node_name_length)),
            std::string::npos)
    << "the message must name the limit";
  EXPECT_NE(error.find("remotes.robot_a.name"), std::string::npos)
    << "the message must name the parameter to fix";

  // A label used as the identity (no `name` set) is checked the same way,
  // and the message points at the label rather than at the unset parameter.
  error.clear();
  auto by_label = resolveRemoteIdentities({{too_long, ""}}, &error);
  EXPECT_TRUE(by_label.empty());
  ASSERT_FALSE(error.empty());
  EXPECT_NE(error.find("remotes_list"), std::string::npos);
}

// The boundary case, which is the one truncation used to break: a name of
// exactly the maximum length is carried on the wire unchanged, so it is
// still the same string on both sides of the loop rule's compare. Before
// #51's rejection policy the local `name` was cut at this length while the
// configured remote identity was not, so a 24-character pair went silently
// unequal and the hub echoed.
TEST(RelayRouting, MaximumLengthIdentityStillClosesTheLoopRule)
{
  const std::string wire_name(udp_bridge::maximum_node_name_length, 'a');
  ASSERT_EQ(wire_name.size(), 23u) << "the wire field is 24 bytes incl. NUL";

  std::string error;
  auto identities = resolveRemoteIdentities({{"robot_a", wire_name}}, &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(identities.at("robot_a"), wire_name)
    << "a name at the limit must be kept whole";

  // A round trip through the wire field must give back the same string --
  // that equality is the loop rule's whole premise.
  udp_bridge::SequencedPacketHeader header;
  memset(&header, 0, sizeof(header));
  memcpy(header.source_node, wire_name.data(), wire_name.size());
  EXPECT_EQ(udp_bridge::wire_field_to_string(header.source_node,
                                             udp_bridge::maximum_node_name_size),
            wire_name);

  // Keyed as on_configure keys it, the sender is excluded from its own
  // traffic: nothing is relayed back.
  std::map<std::string, RemoteDetails> table;
  table[identities.at("robot_a")] = remote("/status", "wifi");
  EXPECT_FALSE(hasRelayDestination(table, wire_name))
    << "a maximum-length name must still match itself on the wire";
  EXPECT_TRUE(selectRateLimitedConnections(table, kT0, wire_name).empty());
}

// A fixed-size wire field need not be null-terminated: a corrupted or
// foreign packet can fill all 24 bytes. Reading it as a C string would run
// off the end of the field -- and off the allocation, if no null byte
// follows. The read is bounded instead.
TEST(RelayRouting, UnterminatedWireNameIsReadWithinItsField)
{
  // The field, filled edge to edge, immediately followed by more non-zero
  // bytes: an unbounded scan would keep going into them.
  struct
  {
    char source_node[udp_bridge::maximum_node_name_size];
    char trailing[8];
  } packed;
  memset(&packed, 'z', sizeof(packed));

  const auto name = udp_bridge::wire_field_to_string(
    packed.source_node, udp_bridge::maximum_node_name_size);
  EXPECT_EQ(name.size(), std::size_t(udp_bridge::maximum_node_name_size))
    << "the read must stop at the end of the field, not at the next null";
  EXPECT_EQ(name, std::string(udp_bridge::maximum_node_name_size, 'z'));

  // And the ordinary null-padded case is unchanged.
  memset(&packed, 0, sizeof(packed));
  memcpy(packed.source_node, "boat", 4);
  EXPECT_EQ(udp_bridge::wire_field_to_string(packed.source_node,
                                             udp_bridge::maximum_node_name_size),
            "boat");
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

  // The probe answering false is what stops it, and this is why that
  // matters: an empty exclude_remote excludes NOBODY, so had the packet
  // reached the selector it would have been fanned out to every remote
  // listing the topic -- the injected-packet amplification of #53. Run on
  // a separate table so the assertion above stays about an untouched one.
  std::map<std::string, RemoteDetails> unguarded;
  unguarded["boat"] = remote("/status", "wifi");
  unguarded["operator"] = remote("/status", "wifi");
  auto would_be_selected = selectRateLimitedConnections(unguarded, kT0, "");
  EXPECT_EQ(selectedRemotes(would_be_selected),
            (std::vector<std::string>{"boat", "operator"}))
    << "with no sender to exclude the selector reaches everyone, sender included";

  // And because the probe refused, nothing in the real table was touched:
  // no remote's last_sent_time was stamped on this packet's behalf.
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
// one connection with period P is due, later connections with period P go
// out with it rather than skewing to their own schedule.
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

// Per-destination field rewriting on the relay path. The hazard is the
// receiver's "destination_topic, else source_topic" fallback: with an empty
// destination_topic (which remoteAdvertise / decodeSubscribeRequest pass
// through unchanged), an unrewritten source_topic resolves downstream to the
// UPSTREAM publisher's topic name, not the hub's. Two boats publishing /nav,
// remapped by the hub, would collide back onto /nav.
TEST(RelayRouting, RelayRewritesSourceTopicToTheHubTopic)
{
  udp_bridge_interfaces::msg::MessageInternal message;
  message.source_topic = "/nav";                   // as boat_a published it
  message.destination_topic = "/boat_a/nav";       // as boat_a addressed it
  message.data = {1, 2, 3};

  // An operator whose config gives no explicit destination topic.
  DestinationConfig config;
  applyRelayDestination(message, "/boat_a/nav", config);

  EXPECT_TRUE(message.destination_topic.empty());
  EXPECT_EQ(message.source_topic, "/boat_a/nav")
    << "the receiver's fallback must land on the hub-local topic";
  EXPECT_EQ(message.data, (std::vector<uint8_t>{1, 2, 3})) << "payload untouched";
}

// With an explicit per-destination topic and QoS, those win; source_topic is
// still the hub-local name.
TEST(RelayRouting, RelayAppliesPerDestinationTopicAndQos)
{
  udp_bridge_interfaces::msg::MessageInternal message;
  message.source_topic = "/nav";
  message.destination_topic = "/boat_a/nav";
  message.reliability = "reliable";
  message.durability = "transient_local";
  message.history_depth = 10;

  DestinationConfig config;
  config.destination_topic = "/operator/boat_a/nav";
  config.reliability = "best_effort";
  config.durability = "volatile";
  config.history_depth = 1;
  applyRelayDestination(message, "/boat_a/nav", config);

  EXPECT_EQ(message.destination_topic, "/operator/boat_a/nav");
  EXPECT_EQ(message.source_topic, "/boat_a/nav");
  EXPECT_EQ(message.reliability, "best_effort");
  EXPECT_EQ(message.durability, "volatile");
  EXPECT_EQ(message.history_depth, 1u);
}

// ...and the grouping is order-dependent, which is worth pinning rather
// than assuming away: a not-yet-due connection encountered BEFORE the first
// due one with the same period is not pulled into the group. Same table as
// the test above with the ids swapped, so iteration (by connection id)
// reaches the recently-sent connection first.
TEST(RelayRouting, SamePeriodGroupingFollowsIterationOrder)
{
  std::map<std::string, RemoteDetails> table;
  RemoteDetails details;
  details.destination_topic = "/status";
  ConnectionRateInfo recent;
  recent.period = 1.0f;
  recent.last_sent_time = kT0;
  ConnectionRateInfo due;
  due.period = 1.0f;
  due.last_sent_time = rclcpp::Time(0, 0, RCL_ROS_TIME);
  details.connection_rates["a"] = recent;  // visited first: not due, skipped
  details.connection_rates["b"] = due;     // visited second: due, selected
  table["operator"] = details;

  auto selected = selectRateLimitedConnections(table, kT0);
  ASSERT_EQ(selected.count("operator"), 1u);
  EXPECT_EQ(selected["operator"], (std::vector<std::string>{"b"}))
    << "grouping only pulls in connections visited after the first due one";
}

// The probe must not answer true for a destination the selector can never
// choose. A negative period means "never send": relaying to such a remote
// costs a payload copy, a relay-queue slot and a worker wakeup for a
// message that is then dropped at selection. Fails if the probe stops
// consulting connection_rates.
TEST(RelayRouting, ProbeAgreesWithSelectorOnNeverSendRemotes)
{
  std::map<std::string, RemoteDetails> table;
  table["boat"] = remote("/status", "wifi");
  table["operator"] = remote("/status", "wifi", -1.0f);  // never send

  EXPECT_TRUE(selectRateLimitedConnections(table, kT0, "boat").empty())
    << "precondition: the selector chooses nobody here";
  EXPECT_FALSE(hasRelayDestination(table, "boat"))
    << "so the probe must not make the drain thread pay for a copy";

  // A second operator that CAN be sent to flips the probe back.
  table["operator_b"] = remote("/status", "wifi");
  EXPECT_TRUE(hasRelayDestination(table, "boat"));
}

// A remote listed for the topic but with no connections at all is likewise
// unreachable: the selector iterates connection_rates and picks nothing.
TEST(RelayRouting, ProbeIgnoresRemotesWithNoConnections)
{
  std::map<std::string, RemoteDetails> table;
  table["boat"] = remote("/status", "wifi");
  RemoteDetails no_connections;
  no_connections.destination_topic = "/status";
  table["operator"] = no_connections;

  EXPECT_TRUE(selectRateLimitedConnections(table, kT0, "boat").empty());
  EXPECT_FALSE(hasRelayDestination(table, "boat"));
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

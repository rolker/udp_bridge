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
//   LabelIsTheIdentity / EmptyRemoteIdentityIsRejected
//                               / RemoteResolvingToOurOwnNameIsRejected
//                               / RepeatedLabelIsIdempotentNotACollision —
//                                 the identity namespace the loop rule
//                                 compares in. There is exactly ONE (#67): a
//                                 `remotes_list` entry IS the name that
//                                 remote calls itself on the wire, so it is
//                                 both the parameter-path label and the key
//                                 the routing table uses, which is what
//                                 arrives in WrappedPacket::source_node.
//                                 (Between #51 and #67 a second namespace
//                                 existed — `remotes.<label>.name` overrode
//                                 the label. That key is REMOVED: a config
//                                 that still sets it fails on_configure,
//                                 pinned in test_node_name_limits.cpp, not
//                                 here.) An entry that cannot mean what the
//                                 config says — empty, or this bridge's own
//                                 name — is refused; a repeated entry is
//                                 idempotent and collapses to one key.
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
// the remote calls itself on the wire.
//
// Issue #51 found that on_configure keyed everything by the `remotes_list`
// label while `remotes.<label>.name` -- documented as the remote's wire
// name -- was declared and never read, so a remote whose own name differed
// from the hub's label for it matched nothing in the loop rule and the hub
// relayed that remote's traffic straight back to it. That happens with a
// SINGLE remote configured -- the configuration relay is supposed to leave
// inert -- which is why it was a correctness bug and not a documentation
// gap.
//
// Issue #67 closed the gap from the other side: the two namespaces were
// never wanted, so a `remotes_list` entry IS the wire name and
// `resolveRemoteIdentities` only validates it. The property below is what
// the rest of these tests rest on.
TEST(RelayRouting, LabelIsTheIdentity)
{
  std::string error;
  auto identities = resolveRemoteIdentities({"robot_a", "operator_1"}, "",
                                            &error);
  EXPECT_TRUE(error.empty()) << error;
  ASSERT_EQ(identities.size(), 2u);
  EXPECT_EQ(identities[0], "robot_a");
  EXPECT_EQ(identities[1], "operator_1");
}

// The empty identity is one of the three a config cannot mean. `""` is a
// reserved sentinel on the send path -- an empty remote name there marks a
// connection request rather than a message to a named remote -- so a remote
// filed under it is silently unroutable, and no arriving packet can match
// it either, because the wire always carries the sender's real name.
TEST(RelayRouting, EmptyRemoteIdentityIsRejected)
{
  std::string error;
  auto identities = resolveRemoteIdentities({""}, "hub", &error);
  EXPECT_TRUE(identities.empty())
    << "an empty remote identity can never be routed to";
  ASSERT_FALSE(error.empty());
  EXPECT_NE(error.find("empty"), std::string::npos)
    << "the message must say what is wrong with the entry";
}

// A remote with the bridge's own name is the second. It installs a
// RemoteNode for ourselves in remote_nodes_, and once that entry exists
// unwrap()'s self-packet refusal -- which sits in the not-found branch --
// is bypassed by the successful lookup, so the bridge starts accepting its
// own traffic as a peer's. RemoteNode's constructor asserts on it, but
// asserts are compiled out of the release builds this would be met in.
// Reject at configure, before the RemoteNode is constructed.
TEST(RelayRouting, RemoteResolvingToOurOwnNameIsRejected)
{
  std::string error;
  auto identities = resolveRemoteIdentities({"hub", "operator_1"}, "hub",
                                            &error);
  EXPECT_TRUE(identities.empty())
    << "a bridge must not be installed as its own remote";
  ASSERT_FALSE(error.empty());
  EXPECT_NE(error.find("hub"), std::string::npos)
    << "the message must name the offending remote";

  // A config that merely resembles it is not rejected: only equality is,
  // so ordinary neighbours still configure.
  error.clear();
  auto ok = resolveRemoteIdentities({"hub_a", "robot_b"}, "hub", &error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(ok.size(), 2u);
}

// A label repeated in `remotes_list` names one parameter block, so it is
// the same remote twice, not two remotes colliding. Before #51 the repeat
// was simply configured twice, idempotently; failing the transition on it
// (with "remotes 'a' and 'a' both resolve to...") would be a regression
// against a config that used to work. Since #67 an exact repeat is the ONLY
// way one identity can appear twice -- two distinct entries are two
// distinct identities by construction -- which is why the function carries
// no duplicate-identity rejection.
TEST(RelayRouting, RepeatedLabelIsIdempotentNotACollision)
{
  std::string error;
  auto identities = resolveRemoteIdentities({"robot_a", "robot_a",
                                             "operator_1"}, "", &error);
  EXPECT_TRUE(error.empty()) << "a remote cannot collide with itself";
  ASSERT_EQ(identities.size(), 2u);
  EXPECT_EQ(identities[0], "robot_a");
  EXPECT_EQ(identities[1], "operator_1");

  // The repeat is collapsed, not carried twice, so downstream keys it once.
  //
  // on_configure's remote-construction loop iterates THIS return value, not
  // the raw remotes_list it was given — otherwise a repeated entry builds and
  // updates the same RemoteNode twice (two DDS publishers created and then
  // discarded, plus a duplicate getaddrinfo) before the second assignment
  // collapses the state. The end state is correct either way, which is why
  // that ran unnoticed until Copilot flagged it on PR #68; keep the loop on
  // the collapsed list so idempotence holds for the work, not just the
  // result. No node-level harness exists to pin the loop itself.
  error.clear();
  auto repeated_only = resolveRemoteIdentities({"robot_a", "robot_a"}, "",
                                               &error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(repeated_only, (std::vector<std::string>{"robot_a"}));
}

// The wire field is `char source_node[24]`, so 23 characters is the longest
// name that survives a round trip. An identity longer than that cannot be
// compared successfully against what actually arrives, so the loop rule
// would be permanently inert for that remote and the hub would echo its
// traffic back to it. Truncating to fit is what manufactures that mismatch
// -- and would let two entries differing only after char 23 collapse onto
// one wire name, silently merging two remotes' connections and rate limits.
// So it is refused, with a message an operator can act on.
TEST(RelayRouting, OverLongRemoteIdentityIsRejected)
{
  const std::string too_long(udp_bridge::maximum_node_name_length + 1, 'x');

  std::string error;
  auto identities = resolveRemoteIdentities({too_long}, "", &error);
  EXPECT_TRUE(identities.empty())
    << "an unrepresentable identity must fail on_configure, not be truncated";
  ASSERT_FALSE(error.empty());
  EXPECT_NE(error.find(too_long), std::string::npos)
    << "the message must quote the offending name";
  EXPECT_NE(error.find(std::to_string(udp_bridge::maximum_node_name_length)),
            std::string::npos)
    << "the message must name the limit";
  EXPECT_NE(error.find("remotes_list"), std::string::npos)
    << "the message must name the parameter to fix";
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
  auto identities = resolveRemoteIdentities({wire_name}, "", &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(identities.size(), 1u);
  ASSERT_EQ(identities[0], wire_name)
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
  table[identities[0]] = remote("/status", "wifi");
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
  // Exactly the field, on the heap, filled edge to edge with no null byte
  // anywhere in it. The allocation ends where the field ends, so an
  // unbounded scan reads past the end of it -- which a sanitizer build
  // reports and an unlucky production run turns into a garbage remote name
  // or a crash.
  std::vector<char> field(udp_bridge::maximum_node_name_size, 'z');

  const auto name = udp_bridge::wire_field_to_string(
    field.data(), udp_bridge::maximum_node_name_size);
  // Bounded at the last byte the encode side can write, which is one short
  // of the field: the trailing byte is the terminator. Reading all 24 would
  // be memory-safe but would yield a 24-character name -- one over the
  // limit on_configure rejects -- and hand it on as a map key and a topic
  // component.
  EXPECT_EQ(name.size(), udp_bridge::maximum_node_name_length)
    << "the read must stop within the field and within the name limit";
  EXPECT_EQ(name, std::string(udp_bridge::maximum_node_name_length, 'z'));

  // And the ordinary null-padded case -- what our own write side produces
  // -- is unchanged.
  std::vector<char> padded(udp_bridge::maximum_node_name_size, '\0');
  memcpy(padded.data(), "boat", 4);
  EXPECT_EQ(udp_bridge::wire_field_to_string(padded.data(),
                                             udp_bridge::maximum_node_name_size),
            "boat");

  // The connection-id field is read the same way, for the same reason.
  std::vector<char> id_field(udp_bridge::maximum_connection_id_size, 'q');
  EXPECT_EQ(udp_bridge::wire_field_to_string(
              id_field.data(), udp_bridge::maximum_connection_id_size),
            std::string(udp_bridge::maximum_connection_id_size - 1, 'q'))
    << "the same canonical bound applies to the connection-id field, so an"
       " unterminated one does not walk newConnection's truncation path";
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

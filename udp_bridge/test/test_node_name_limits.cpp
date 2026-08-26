// Node-name length is a hard limit, enforced at every configuration entry
// point (issue #51).
//
// The wire carries `char source_node[maximum_node_name_size]` (24 bytes),
// so 23 characters is the longest node name that survives a round trip.
// The bridge used to TRUNCATE a longer local name and warn about it, while
// leaving a configured remote identity untruncated. That is what
// manufactured the failure the relay loop rule cannot tolerate: the rule is
// a string compare between the wire `source_node` and the configured
// identity, so shortening one side and not the other silently made two
// things that are supposed to be equal unequal — the rule went inert and
// the hub echoed a sender's own traffic back to it. Truncating BOTH sides
// would not have been enough either: two configured names differing only
// after character 23 would then collapse onto one wire identity, silently
// merging two remotes' connections and rate limits.
//
// So an over-long name is rejected, loudly, and never shortened. These
// tests drive the real lifecycle transition rather than the predicates
// underneath it, because the thing worth pinning is that the node refuses
// to come up — a config that used to start with a warning now fails, which
// is a deliberate behaviour change and needs a regression guard.
//
// `resolveRemoteIdentities`' own rejection, the boundary round trip through
// the wire field, and the bounded read of an unterminated field are unit
// tested in test_relay_routing.cpp.

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lifecycle_msgs/msg/state.hpp"
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/packet.h"
#include "udp_bridge/udp_bridge.h"
#include "udp_bridge_interfaces/srv/add_remote.hpp"
#include "udp_bridge_interfaces/srv/list_remotes.hpp"

namespace
{

using udp_bridge::maximum_node_name_length;

const std::string kTooLong(maximum_node_name_length + 1, 'x');
const std::string kAtLimit(maximum_node_name_length, 'a');
const std::string kOtherAtLimit(maximum_node_name_length, 'b');

// A bridge driven through the REAL configuration entry point.
//
// Every value below becomes an `rclcpp::NodeOptions` parameter OVERRIDE —
// byte-for-byte the route a params YAML file or a `-p` command-line
// override takes — and the node is not constructed until `configure()`,
// because overrides are fixed at construction. Nothing here pre-declares a
// parameter.
//
// That is a rule, not a style choice (#52, review round 3). The previous
// harness pre-declared each key with `node_->declare_parameter(...)`, which
// SHORT-CIRCUITS `on_configure`'s `declareIfMissing` and moves any type
// mismatch from `declare_parameter` to the later read. A test written that
// way certified an integer-literal guard as working when the field path —
// where the override reaches an undeclared parameter and
// `declare_parameter` itself throws — still failed the whole lifecycle
// transition. A harness that can establish a state no config file can
// produce is not testing the configuration entry point.
//
// `port` is 0 on purpose: the kernel assigns an ephemeral port, so a test
// that reaches the bind cannot collide with a real bridge (or with another
// test) on 4200 — and on_configure calls exit(1) if the bind fails.
class Bridge
{
public:
  explicit Bridge(const std::string& node_name)
  : node_name_(node_name)
  {
    overrides_.emplace_back("port", 0);
  }

  Bridge& name(const std::string& value)
  {
    overrides_.emplace_back("name", value);
    return *this;
  }

  // `label` is the remote's identity: since #67 a remotes_list entry IS
  // the name that remote calls itself on the wire. `retired_name` sets the
  // removed `remotes.<label>.name` key, which on_configure declares only so
  // rclcpp surfaces the override and it can REFUSE a config that still
  // carries it. Pass "" for every test that is not specifically about that
  // refusal -- an empty value is indistinguishable from an absent key.
  Bridge& remote(const std::string& label, const std::string& retired_name)
  {
    labels_.push_back(label);
    overrides_.emplace_back("remotes." + label + ".name", retired_name);
    return *this;
  }

  // Declare a connection under `label`, so on_configure walks the
  // per-connection parameter block. Loopback host/port: on_configure
  // resolves the host, and the connection is never sent on here.
  Bridge& connection(const std::string& label, const std::string& connection_id)
  {
    connections_[label].push_back(connection_id);
    const std::string base = "remotes." + label + ".connections." + connection_id + ".";
    overrides_.emplace_back(base + "host", "127.0.0.1");
    overrides_.emplace_back(base + "port", 4201);
    return *this;
  }

  // Set one per-connection parameter with a caller-chosen TYPE — which is
  // the point for the tests below: `5` and `5.0` are different ROS 2
  // parameter types for the same YAML-looking value, and as an OVERRIDE
  // (not a pre-declaration) the integer one reaches `declare_parameter`
  // exactly as it would from a config file.
  template<typename T>
  Bridge& connectionParameter(const std::string& label, const std::string& connection_id,
                              const std::string& key, const T& value)
  {
    overrides_.emplace_back(
      "remotes." + label + ".connections." + connection_id + "." + key, value);
    return *this;
  }

  // Add a topic to a connection, so on_configure walks the per-topic
  // parameter block (`period` among them).
  Bridge& topic(const std::string& label, const std::string& connection_id,
                const std::string& topic_name)
  {
    topics_["remotes." + label + ".connections." + connection_id + "."]
      .push_back(topic_name);
    return *this;
  }

  // Set one per-topic parameter with a caller-chosen TYPE, as an override.
  template<typename T>
  Bridge& topicParameter(const std::string& label, const std::string& connection_id,
                         const std::string& topic_name, const std::string& key,
                         const T& value)
  {
    overrides_.emplace_back(
      "remotes." + label + ".connections." + connection_id + ".topics." +
      topic_name + "." + key, value);
    return *this;
  }

  // Returns the state id the node lands in. A configure that fails leaves
  // it UNCONFIGURED; one that succeeds leaves it INACTIVE.
  uint8_t configure()
  {
    auto overrides = overrides_;
    overrides.emplace_back("remotes_list", labels_);
    for(const auto& entry: connections_)
      overrides.emplace_back(
        "remotes." + entry.first + ".connections_list", entry.second);
    for(const auto& entry: topics_)
      overrides.emplace_back(entry.first + "topics_list", entry.second);
    node_ = std::make_shared<udp_bridge::UDPBridge>(
      node_name_, rclcpp::NodeOptions().parameter_overrides(overrides));
    return node_->configure().id();
  }

  // Valid only after configure() — the node does not exist before it.
  std::shared_ptr<udp_bridge::UDPBridge> node() const { return node_; }

private:
  std::string node_name_;
  std::shared_ptr<udp_bridge::UDPBridge> node_;
  std::vector<rclcpp::Parameter> overrides_;
  std::vector<std::string> labels_;
  std::map<std::string, std::vector<std::string>> connections_;
  std::map<std::string, std::vector<std::string>> topics_;
};

constexpr uint8_t kUnconfigured = lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED;
constexpr uint8_t kInactive = lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;

}  // namespace

// The local name. It used to be cut to 23 characters with a WARN, so the
// bridge came up answering to a name no remote was configured with.
TEST(NodeNameLimits, OverLongLocalNameFailsToConfigure)
{
  Bridge bridge("over_long_local_name");
  EXPECT_EQ(bridge.name(kTooLong).configure(), kUnconfigured)
    << "an over-long `name` must fail the transition, not be truncated";
}

// A configured remote identity. Untruncated before #51, so it could never
// equal the (truncated) name arriving on the wire from that remote. Since
// #67 the `remotes_list` entry is the only way to spell that identity, so
// this one test covers the whole configure-time path.
TEST(NodeNameLimits, OverLongRemoteLabelFailsToConfigure)
{
  Bridge bridge("over_long_remote_label");
  EXPECT_EQ(bridge.name("hub").remote(kTooLong, "").configure(), kUnconfigured);
}

// The boundary, which is the case truncation used to break: names of
// exactly the maximum length are carried whole, so the bridge configures.
TEST(NodeNameLimits, MaximumLengthNamesConfigure)
{
  Bridge bridge("maximum_length_names");
  EXPECT_EQ(bridge.name(kAtLimit).remote(kOtherAtLimit, "").configure(),
            kInactive)
    << "a name at the limit is representable and must be accepted";
}

// A remote configured with this bridge's own name used to be installed in
// remote_nodes_ under our name, at which point unwrap()'s self-packet
// refusal (which only runs when the lookup misses) stopped protecting us.
// RemoteNode's constructor asserts on it -- and asserts are compiled out of
// release builds, so the field build was the one without the guard. It now
// fails the transition instead. Since #67 the `remotes_list` entry is the
// only way to spell a remote's identity, so this one test covers the whole
// configure-time path.
TEST(NodeNameLimits, RemoteLabelMatchingOurOwnNameFailsToConfigure)
{
  Bridge bridge("remote_label_matching_our_name");
  EXPECT_EQ(bridge.name("hub").remote("hub", "").configure(), kUnconfigured)
    << "a bridge must not be configured as its own remote";
}

// `remotes.<label>.name` was REMOVED in #67. It is still declared, because
// rclcpp surfaces a YAML override only for a declared parameter -- which is
// the only way a config that still carries the key can be detected at all.
// A non-empty value fails the transition: warning would leave the one shape
// that reintroduces the #51 echo (a value differing from its label) merely
// announced, while every other identity error refuses to come up.
//
// These drive the real lifecycle transition. The refusal is directly
// observable in the state the node lands in, so no test accessor is needed
// (the earlier warn-only behaviour needed one, since nothing in this
// package captures log content).
TEST(NodeNameLimits, RetiredRemoteNameParameterFailsToConfigure)
{
  Bridge bridge("retired_remote_name_param");
  EXPECT_EQ(bridge.name("hub").remote("robot_a", "some_other_name")
              .configure(),
            kUnconfigured)
    << "a removed, unsupported parameter must fail the transition, not be"
       " honoured or ignored";
}

// No carve-out for the redundant case. A stale value equal to its label
// changes no identity, but the parameter is removed and unsupported, so
// setting it at all is a configuration error -- and one uniform rule is
// what an operator can hold in their head.
TEST(NodeNameLimits, RetiredRemoteNameMatchingItsLabelStillFailsToConfigure)
{
  Bridge bridge("retired_name_equals_label");
  EXPECT_EQ(bridge.name("hub").remote("robot_a", "robot_a").configure(),
            kUnconfigured)
    << "the retired key is rejected on presence, not on whether it would"
       " have changed the identity";
}

// Every offending key is reported in one ERROR, so a hub carrying several
// stale keys is not a restart-per-key discovery loop. Only the state is
// observable from here (nothing in this package captures log content), so
// what this pins is that the accumulate-then-fail loop still refuses a
// config with more than one offender -- the shape a first-offender `return`
// inside the loop would also satisfy, but a broken accumulator would not.
TEST(NodeNameLimits, SeveralRetiredRemoteNameParametersFailToConfigure)
{
  Bridge bridge("several_retired_name_params");
  EXPECT_EQ(bridge.name("hub")
              .remote("robot_a", "some_other_name")
              .remote("robot_b", "yet_another_name")
              .configure(),
            kUnconfigured)
    << "a config carrying more than one removed key must fail the"
       " transition";
}

// The counterpart, and the reason the rejection is a real assertion: an
// absent key is the normal case and must configure cleanly. An explicitly
// empty value is indistinguishable from an absent one -- the declared
// default is the empty string -- so this covers both.
TEST(NodeNameLimits, AbsentRemoteNameParameterConfiguresCleanly)
{
  Bridge bridge("absent_remote_name_parameter");
  EXPECT_EQ(bridge.name("hub").remote("robot_a", "").configure(), kInactive)
    << "a config that does not set the retired key must come up";
}

// The third member of the entry-validation family (over-long and self-name
// have had lifecycle tests since #51). An empty entry is the one input that
// makes on_configure declare `"remotes..name"` before validating, so the
// step is worth driving through the real transition rather than only
// through resolveRemoteIdentities in test_relay_routing.cpp.
TEST(NodeNameLimits, EmptyRemoteLabelFailsToConfigure)
{
  Bridge bridge("empty_remote_label");
  EXPECT_EQ(bridge.name("hub").remote("", "").configure(), kUnconfigured)
    << "the empty name is a send-path sentinel; a remote filed under it"
       " could never be routed to";
}

// `add_remote` is the one path that creates a remote at runtime from an
// arbitrary caller-supplied string, so it needs the same self-name refusal
// `resolveRemoteIdentities` performs at configure time -- otherwise the
// invariant the configure-time check establishes (nothing in remote_nodes_
// is keyed by our own name) can still be broken through the service, and
// unwrap()'s self-packet guard, which only runs when the lookup misses,
// stops protecting us.
//
// Driven through the real services: add_remote to attempt it, list_remotes
// to observe remote_nodes_ from outside the class.
TEST(NodeNameLimits, AddRemoteServiceRefusesOurOwnName)
{
  const std::string kNodeName = "add_remote_self_name";
  Bridge bridge(kNodeName);
  ASSERT_EQ(bridge.name("hub").configure(), kInactive);

  auto client_node = std::make_shared<rclcpp::Node>(kNodeName + "_client");
  auto add = client_node->create_client<udp_bridge_interfaces::srv::AddRemote>(
    "/" + kNodeName + "/add_remote");
  auto list = client_node->create_client<udp_bridge_interfaces::srv::ListRemotes>(
    "/" + kNodeName + "/list_remotes");

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(bridge.node()->get_node_base_interface());
  executor.add_node(client_node);
  std::thread spinner([&executor](){ executor.spin(); });

  ASSERT_TRUE(add->wait_for_service(std::chrono::seconds(10)));
  ASSERT_TRUE(list->wait_for_service(std::chrono::seconds(10)));

  auto call_add = [&](const std::string& name)
  {
    auto request = std::make_shared<udp_bridge_interfaces::srv::AddRemote::Request>();
    request->name = name;
    request->address = "127.0.0.1";
    request->port = 4200;
    auto future = add->async_send_request(request);
    return future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  };

  auto remote_names = [&]()
  {
    auto future = list->async_send_request(
      std::make_shared<udp_bridge_interfaces::srv::ListRemotes::Request>());
    std::vector<std::string> names;
    if(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready)
    {
      // Hold the response alive: get() hands back the shared_ptr by value,
      // so iterating `future.get()->remotes` directly would free it at the
      // end of the expression and walk a dangling vector.
      auto response = future.get();
      for(const auto& remote: response->remotes)
        names.push_back(remote.name);
    }
    return names;
  };

  // Control: an ordinary remote really is created through this service, so
  // the absence asserted below is a refusal and not a broken test.
  ASSERT_TRUE(call_add("robot_a"));
  {
    auto names = remote_names();
    EXPECT_NE(std::find(names.begin(), names.end(), "robot_a"), names.end())
      << "add_remote must create an ordinary remote";
  }

  // The refusal: our own name must never appear in remote_nodes_.
  ASSERT_TRUE(call_add("hub"));
  {
    auto names = remote_names();
    EXPECT_EQ(std::find(names.begin(), names.end(), "hub"), names.end())
      << "add_remote must refuse a remote named as this bridge is named";
  }

  executor.cancel();
  spinner.join();
}

// `link_headroom_fraction` is RETIRED (#52): the `(1 - headroom) x
// goodput` clamp it fed was removed on 2026-08-25, and nothing stores it
// any more. It is still DECLARED, purely so a stale key in an existing
// config is visible — rclcpp surfaces a YAML override only for a
// declared parameter, so undeclaring it would make the key silently
// ignored, which is the "set it, nothing happens" shape #52 exists to
// remove.
//
// Unlike the retired `remotes.<label>.name`, a stale value here must NOT
// fail the transition. That key misroutes traffic when stale (the #51
// echo bug); this one misroutes nothing, so refusing to configure would
// ground a boat over a dead config key. One WARN, and the node comes up.
TEST(RetiredParameters, LinkHeadroomFractionStillConfigures)
{
  Bridge bridge("retired_headroom");
  bridge.name("hub").remote("peer", "").connection("peer", "c0");
  bridge.connectionParameter("peer", "c0", "link_headroom_fraction", 0.5);
  EXPECT_EQ(bridge.configure(), kInactive)
    << "a config still setting the retired link_headroom_fraction must "
       "WARN and come up, not fail the transition";
}

// The double-literal trap, guarded at the DECLARATION (#52).
//
// YAML has no way to say "this integer is a double", so
// `admission_refractory_period_seconds: 5` arrives as a ROS 2 INTEGER
// override for a parameter whose default is a double. Under static
// parameter typing that combination throws
// `InvalidParameterTypeException` inside `declare_parameter` — i.e.
// before any read of the value — and the throw lands in on_configure,
// where rclcpp_lifecycle SWALLOWS it and leaves a node that silently
// failed to configure with nothing in the log naming the parameter.
//
// This drives the real entry point: every value below is a NodeOptions
// parameter OVERRIDE, so `declare_parameter` sees exactly what it sees
// from a params file. Round 2's version of this test pre-declared the
// key as an INTEGER, which short-circuits `declareIfMissing` entirely
// and moved the failure to the read — a state no config file can reach.
// It passed while `admission_refractory_period_seconds: 5` in a real
// config still failed the whole transition.
TEST(RetiredParameters, IntegerLiteralForADoubleParameterConfigures)
{
  Bridge bridge("integer_double_literal");
  bridge.name("hub").remote("peer", "").connection("peer", "c0");
  bridge.connectionParameter("peer", "c0", "admission_refractory_period_seconds", 5);
  bridge.connectionParameter("peer", "c0", "admission_floor_bytes_per_second", 9000);
  bridge.connectionParameter("peer", "c0", "resend_budget_fraction", 1);
  bridge.topic("peer", "c0", "chatter");
  bridge.topicParameter("peer", "c0", "chatter", "period", 1);
  ASSERT_EQ(bridge.configure(), kInactive)
    << "an integer literal for a double-typed connection parameter must "
       "be coerced at declaration, not thrown out of the lifecycle "
       "transition where the exception is swallowed and the failure is "
       "invisible";

  // Coerced, not merely survived: the parameter must end up DOUBLE-typed
  // and carrying the configured magnitude. A guard that swallowed the
  // override and installed the default would also reach INACTIVE.
  const std::string base = "remotes.peer.connections.c0.";
  for(const auto& expected: std::map<std::string, double>{
        {base + "admission_refractory_period_seconds", 5.0},
        {base + "admission_floor_bytes_per_second", 9000.0},
        {base + "resend_budget_fraction", 1.0},
        {base + "topics.chatter.period", 1.0}})
  {
    const auto parameter = bridge.node()->get_parameter(expected.first);
    EXPECT_EQ(parameter.get_type(), rclcpp::ParameterType::PARAMETER_DOUBLE)
      << expected.first << " must stay statically typed as a double";
    EXPECT_DOUBLE_EQ(parameter.as_double(), expected.second)
      << expected.first << " must carry the value the config asked for";
  }
}

// The same parameters written the ordinary way still work — the coercion
// path must not have displaced the normal one.
TEST(RetiredParameters, DoubleLiteralForADoubleParameterConfigures)
{
  Bridge bridge("double_double_literal");
  bridge.name("hub").remote("peer", "").connection("peer", "c0");
  bridge.connectionParameter("peer", "c0", "admission_refractory_period_seconds", 5.5);
  bridge.connectionParameter("peer", "c0", "admission_floor_bytes_per_second", 9000.5);
  ASSERT_EQ(bridge.configure(), kInactive);
  EXPECT_DOUBLE_EQ(
    bridge.node()->get_parameter(
      "remotes.peer.connections.c0.admission_refractory_period_seconds").as_double(),
    5.5);
}

// A genuinely wrong type is still refused. INTEGER is coerced because
// there is one defensible reading of `5` as a rate; a string has none,
// so the transition must fail rather than silently install a default.
TEST(RetiredParameters, StringLiteralForADoubleParameterFailsToConfigure)
{
  Bridge bridge("string_double_literal");
  bridge.name("hub").remote("peer", "").connection("peer", "c0");
  bridge.connectionParameter("peer", "c0", "admission_refractory_period_seconds",
                             std::string("five"));
  EXPECT_EQ(bridge.configure(), kUnconfigured)
    << "a string where a rate belongs must not be silently accepted";
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

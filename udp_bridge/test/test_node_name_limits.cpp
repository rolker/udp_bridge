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

// A bridge with its parameters pre-declared, so on_configure's
// declareIfMissing picks them up instead of installing defaults.
//
// `port` is 0 on purpose: the kernel assigns an ephemeral port, so a test
// that reaches the bind cannot collide with a real bridge (or with another
// test) on 4200 — and on_configure calls exit(1) if the bind fails.
class Bridge
{
public:
  explicit Bridge(const std::string& node_name)
  : node_(std::make_shared<udp_bridge::UDPBridge>(node_name))
  {
    node_->declare_parameter("port", 0);
  }

  Bridge& name(const std::string& value)
  {
    node_->declare_parameter("name", value);
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
    node_->declare_parameter("remotes." + label + ".name", retired_name);
    return *this;
  }

  // Returns the state id the node lands in. A configure that fails leaves
  // it UNCONFIGURED; one that succeeds leaves it INACTIVE.
  uint8_t configure()
  {
    node_->declare_parameter("remotes_list", labels_);
    return node_->configure().id();
  }

  std::shared_ptr<udp_bridge::UDPBridge> node() const { return node_; }

private:
  std::shared_ptr<udp_bridge::UDPBridge> node_;
  std::vector<std::string> labels_;
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

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  rclcpp::init(0, nullptr);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

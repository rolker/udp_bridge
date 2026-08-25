// Per-connection tunables must apply to the LIVE connection when set at
// runtime, not only when read once in on_configure.
//
// Field failure this pins (BizzyBoat, 2026-08-25 transit): the VPN
// connection's admission control had collapsed, and the operator raised
// `remotes.<r>.connections.<c>.admission_floor_bytes_per_second` with
// `ros2 param set`. rclcpp accepted it, `ros2 param get` read the new
// value back, and nothing changed — the parameter was declared and read
// exactly once, in on_configure, and the OnSetParameters callback only
// ever handled the two resend-give-up thresholds. The wrong conclusion
// that followed ("the floor is not the problem") cost about three
// minutes of link outage.
//
// A parameter that is accepted, reads back, and does nothing is worse
// than one that does not exist: it answers the operator's question with
// a lie. So the tests below assert against the live `Connection` object
// that meters the link, reached through UDPBridge::connectionForTest,
// rather than against the parameter store.
//
// Pinned contract points:
//
//   AdmissionFloorAppliesToLiveConnection — the headline regression.
//   MaximumBytesPerSecondAppliesToLiveConnection
//   LinkHeadroomFractionAppliesToLiveConnection
//   ResendBudgetFractionAppliesToLiveConnection
//                               — the sibling tunables, same defect.
//   NonFiniteFloorRejected / NegativeFloorRejected
//                               — refused with a reason naming the
//                                 parameter, and the live value is left
//                                 alone. A runtime set has a return
//                                 channel, so it rejects where
//                                 on_configure clamps (see
//                                 connection_tunables.h).
//   OutOfRangeHeadroomRejected  — the accepted range is the setter's
//                                 clamp range, so an accepted value is
//                                 stored unchanged and `param get`
//                                 cannot disagree with what is in force.
//   BadValueInBatchAppliesNothing — validate-whole-batch-then-apply: one
//                                 bad value leaves every other value in
//                                 the batch untouched.
//   GiveupThresholdsStillValidatedAsAPair — the pre-existing behaviour
//                                 of this callback, preserved across the
//                                 rewrite that added the tunables.
//   UnrelatedParameterUntouched — an unknown/unregistered parameter is
//                                 not rejected (rclcpp calls that bad
//                                 practice) and not applied.
//
// The validate*() helpers are exercised directly too: they are pure and
// need no node, and they are what a future entry point (a service, a
// second callback) would reuse.

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lifecycle_msgs/msg/state.hpp"
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/connection_tunables.h"
#include "udp_bridge/resend_constants.h"
#include "udp_bridge/udp_bridge.h"

using udp_bridge::TunableValidation;
using udp_bridge::validateAdmissionFloorBytesPerSecond;
using udp_bridge::validateLinkHeadroomFraction;
using udp_bridge::validateMaximumBytesPerSecond;
using udp_bridge::validateResendBudgetFraction;

namespace
{

const std::string kRemote = "robot_a";
// Seven characters or fewer: longer connection ids are canonicalized by
// truncate_connection_id, and the parameter path would then name a
// connection id the live map is not keyed by.
const std::string kConnection = "vpn";

constexpr int64_t kConfiguredRateLimit = 500000;
constexpr double kConfiguredFloor = 8192.0;
constexpr double kConfiguredHeadroom = 0.2;
constexpr double kConfiguredResendBudget = 0.25;

std::string connectionParam(const std::string& key)
{
  return "remotes." + kRemote + ".connections." + kConnection + "." + key;
}

// A bridge configured with one remote and one connection, with every
// tunable declared up front so on_configure's declareIfMissing adopts
// these values instead of installing defaults.
//
// `port` is 0 on purpose, exactly as in test_node_name_limits.cpp: the
// kernel assigns an ephemeral port, so a test that reaches the bind can
// never collide with a real bridge on 4200 — on_configure calls exit(1)
// if the bind fails. The connection's destination is loopback with a
// port nothing listens on; no packet is ever sent by these tests, the
// address only has to resolve.
class Bridge
{
public:
  explicit Bridge(const std::string& node_name)
  : node_(std::make_shared<udp_bridge::UDPBridge>(node_name))
  {
    node_->declare_parameter("port", 0);
    node_->declare_parameter("name", std::string("test_bridge"));
    node_->declare_parameter("remotes_list", std::vector<std::string>{kRemote});
    node_->declare_parameter("remotes." + kRemote + ".connections_list",
                             std::vector<std::string>{kConnection});
    node_->declare_parameter(connectionParam("host"), std::string("127.0.0.1"));
    node_->declare_parameter(connectionParam("port"), 45999);
    node_->declare_parameter(connectionParam("maximum_bytes_per_second"),
                             kConfiguredRateLimit);
    node_->declare_parameter(connectionParam("admission_floor_bytes_per_second"),
                             kConfiguredFloor);
    node_->declare_parameter(connectionParam("link_headroom_fraction"),
                             kConfiguredHeadroom);
    node_->declare_parameter(connectionParam("resend_budget_fraction"),
                             kConfiguredResendBudget);
  }

  // Configure and hand back the live Connection. Both are assertions in
  // the caller's frame via the ASSERT_* in configure(); returns nullptr
  // if either step failed.
  std::shared_ptr<udp_bridge::Connection> configure()
  {
    if(node_->configure().id() !=
       lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE)
      return nullptr;
    return node_->connectionForTest(kRemote, kConnection);
  }

  udp_bridge::UDPBridge& node() { return *node_; }

private:
  std::shared_ptr<udp_bridge::UDPBridge> node_;
};

}  // namespace

// ---------------------------------------------------------------------
// Pure validation. No node, no socket.
// ---------------------------------------------------------------------

TEST(TunableValidationRules, AdmissionFloorAcceptsAnyFiniteNonNegative)
{
  EXPECT_TRUE(validateAdmissionFloorBytesPerSecond("floor", 0.0).ok);
  EXPECT_TRUE(validateAdmissionFloorBytesPerSecond("floor", 8192.0).ok);
  // No upper bound relative to the connection's own cap: the floor is
  // clamped AT USE, so a floor above the cap yields the cap and a cap
  // raised later is not met by a silently truncated floor.
  EXPECT_TRUE(validateAdmissionFloorBytesPerSecond("floor", 1.0e9).ok);
}

TEST(TunableValidationRules, AdmissionFloorRejectsNonFiniteAndNegative)
{
  for(double bad: {std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::infinity(),
                   -1.0})
  {
    auto v = validateAdmissionFloorBytesPerSecond("the_floor_param", bad);
    EXPECT_FALSE(v.ok) << "value " << bad << " must be refused";
    EXPECT_NE(v.reason.find("the_floor_param"), std::string::npos)
      << "the reason must name the parameter so `ros2 param set` output "
         "is actionable; got: " << v.reason;
  }
  // Beyond float range the cast to Connection's float field would yield
  // inf, so the getter and the parameter store would disagree.
  EXPECT_FALSE(validateAdmissionFloorBytesPerSecond("floor", 1.0e300).ok);
}

TEST(TunableValidationRules, HeadroomAcceptsExactlyWhatTheSetterKeeps)
{
  EXPECT_TRUE(validateLinkHeadroomFraction("headroom", 0.0).ok);
  EXPECT_TRUE(validateLinkHeadroomFraction("headroom", 0.5).ok);
  EXPECT_TRUE(validateLinkHeadroomFraction(
    "headroom", static_cast<double>(udp_bridge::kMaxLinkHeadroomFraction)).ok);
  // 1.0 would target zero throughput on every congested sample.
  EXPECT_FALSE(validateLinkHeadroomFraction("headroom", 1.0).ok);
  EXPECT_FALSE(validateLinkHeadroomFraction("headroom", -0.1).ok);
  EXPECT_FALSE(validateLinkHeadroomFraction(
    "headroom", std::numeric_limits<double>::quiet_NaN()).ok);
}

TEST(TunableValidationRules, ResendBudgetFractionIsAFraction)
{
  EXPECT_TRUE(validateResendBudgetFraction("budget", 0.0).ok);
  EXPECT_TRUE(validateResendBudgetFraction("budget", 1.0).ok);
  EXPECT_FALSE(validateResendBudgetFraction("budget", 1.5).ok);
  EXPECT_FALSE(validateResendBudgetFraction("budget", -0.01).ok);
  EXPECT_FALSE(validateResendBudgetFraction(
    "budget", std::numeric_limits<double>::infinity()).ok);
}

TEST(TunableValidationRules, MaximumBytesPerSecondFitsUint32)
{
  // 0 keeps its established meaning (Connection::default_rate_limit), so
  // it is accepted rather than treated as "stop sending".
  EXPECT_TRUE(validateMaximumBytesPerSecond("cap", 0).ok);
  EXPECT_TRUE(validateMaximumBytesPerSecond("cap", 1500000).ok);
  EXPECT_TRUE(validateMaximumBytesPerSecond(
    "cap", std::numeric_limits<uint32_t>::max()).ok);
  EXPECT_FALSE(validateMaximumBytesPerSecond("cap", -1).ok);
  EXPECT_FALSE(validateMaximumBytesPerSecond(
    "cap", static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1).ok);
}

// ---------------------------------------------------------------------
// Runtime application, against the live Connection.
// ---------------------------------------------------------------------

TEST(RuntimeTunables, AdmissionFloorAppliesToLiveConnection)
{
  Bridge bridge("runtime_floor");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr) << "the test bridge must configure";
  ASSERT_FLOAT_EQ(connection->admissionFloorBytesPerSecond(),
                  static_cast<float>(kConfiguredFloor));

  const std::string name = connectionParam("admission_floor_bytes_per_second");
  auto result = bridge.node().set_parameter(rclcpp::Parameter(name, 20000.0));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_FLOAT_EQ(connection->admissionFloorBytesPerSecond(), 20000.0f)
    << "a runtime set must reach the Connection that meters the link, "
       "not only the parameter store";
  EXPECT_DOUBLE_EQ(bridge.node().get_parameter(name).as_double(), 20000.0);
}

TEST(RuntimeTunables, MaximumBytesPerSecondAppliesToLiveConnection)
{
  Bridge bridge("runtime_rate_limit");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);
  ASSERT_EQ(connection->rateLimit(),
            static_cast<uint32_t>(kConfiguredRateLimit));

  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(connectionParam("maximum_bytes_per_second"), int64_t{250000}));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_EQ(connection->rateLimit(), 250000u);
  // No backoff has accumulated, so setRateLimit's follow branch applies
  // and the metered cap moves with the configured one.
  EXPECT_EQ(connection->effectiveRateLimit(), 250000u);
}

TEST(RuntimeTunables, LinkHeadroomFractionAppliesToLiveConnection)
{
  Bridge bridge("runtime_headroom");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);
  ASSERT_FLOAT_EQ(connection->linkHeadroomFraction(),
                  static_cast<float>(kConfiguredHeadroom));

  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(connectionParam("link_headroom_fraction"), 0.4));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_FLOAT_EQ(connection->linkHeadroomFraction(), 0.4f);
}

TEST(RuntimeTunables, ResendBudgetFractionAppliesToLiveConnection)
{
  Bridge bridge("runtime_resend_budget");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);
  ASSERT_FLOAT_EQ(connection->resendBudgetFraction(),
                  static_cast<float>(kConfiguredResendBudget));

  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(connectionParam("resend_budget_fraction"), 0.1));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_FLOAT_EQ(connection->resendBudgetFraction(), 0.1f);
}

TEST(RuntimeTunables, NonFiniteFloorRejectedAndLiveValueUntouched)
{
  Bridge bridge("runtime_floor_nan");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);

  const std::string name = connectionParam("admission_floor_bytes_per_second");
  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(name, std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(result.successful);
  EXPECT_NE(result.reason.find(name), std::string::npos)
    << "reason should name the parameter; got: " << result.reason;
  EXPECT_FLOAT_EQ(connection->admissionFloorBytesPerSecond(),
                  static_cast<float>(kConfiguredFloor));
  EXPECT_DOUBLE_EQ(bridge.node().get_parameter(name).as_double(),
                   kConfiguredFloor);
}

TEST(RuntimeTunables, NegativeFloorRejectedRatherThanSilentlyDisablingTheFloor)
{
  Bridge bridge("runtime_floor_negative");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);

  auto result = bridge.node().set_parameter(rclcpp::Parameter(
    connectionParam("admission_floor_bytes_per_second"), -1.0));
  EXPECT_FALSE(result.successful);
  // Clamping a negative to 0 would turn the floor OFF — the one outcome
  // the floor exists to prevent (see Connection::setAdmissionFloorBytesPerSecond).
  EXPECT_FLOAT_EQ(connection->admissionFloorBytesPerSecond(),
                  static_cast<float>(kConfiguredFloor));
}

TEST(RuntimeTunables, OutOfRangeHeadroomRejected)
{
  Bridge bridge("runtime_headroom_range");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);

  auto result = bridge.node().set_parameter(
    rclcpp::Parameter(connectionParam("link_headroom_fraction"), 1.0));
  EXPECT_FALSE(result.successful);
  EXPECT_FLOAT_EQ(connection->linkHeadroomFraction(),
                  static_cast<float>(kConfiguredHeadroom));
}

TEST(RuntimeTunables, NegativeRateLimitRejected)
{
  Bridge bridge("runtime_rate_limit_negative");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);

  auto result = bridge.node().set_parameter(rclcpp::Parameter(
    connectionParam("maximum_bytes_per_second"), int64_t{-1}));
  EXPECT_FALSE(result.successful);
  EXPECT_EQ(connection->rateLimit(),
            static_cast<uint32_t>(kConfiguredRateLimit));
}

TEST(RuntimeTunables, BadValueInBatchAppliesNothing)
{
  Bridge bridge("runtime_batch");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);

  // A good floor and an impossible headroom in one atomic set.
  auto result = bridge.node().set_parameters_atomically({
    rclcpp::Parameter(connectionParam("admission_floor_bytes_per_second"), 20000.0),
    rclcpp::Parameter(connectionParam("link_headroom_fraction"), 5.0),
  });
  EXPECT_FALSE(result.successful);
  EXPECT_FLOAT_EQ(connection->admissionFloorBytesPerSecond(),
                  static_cast<float>(kConfiguredFloor))
    << "the whole batch is validated before any of it is applied";
  EXPECT_FLOAT_EQ(connection->linkHeadroomFraction(),
                  static_cast<float>(kConfiguredHeadroom));
}

TEST(RuntimeTunables, WholeValidBatchApplies)
{
  Bridge bridge("runtime_batch_ok");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);

  auto result = bridge.node().set_parameters_atomically({
    rclcpp::Parameter(connectionParam("admission_floor_bytes_per_second"), 30000.0),
    rclcpp::Parameter(connectionParam("link_headroom_fraction"), 0.35),
  });
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_FLOAT_EQ(connection->admissionFloorBytesPerSecond(), 30000.0f);
  EXPECT_FLOAT_EQ(connection->linkHeadroomFraction(), 0.35f);
}

// The behaviour this callback had before the tunables were added. The
// give-up thresholds are validated as a PAIR against the current values,
// so a set of one is checked against the other.
TEST(RuntimeTunables, GiveupThresholdsStillValidatedAsAPair)
{
  Bridge bridge("runtime_giveups");
  ASSERT_TRUE(bridge.configure() != nullptr);

  // warn defaults to 5.0, error to 50.0. Raising warn above error must
  // be refused; a value below it must be accepted.
  auto bad = bridge.node().set_parameter(
    rclcpp::Parameter("resend_giveup_warn_rate_per_s", 500.0));
  EXPECT_FALSE(bad.successful);

  auto good = bridge.node().set_parameter(
    rclcpp::Parameter("resend_giveup_warn_rate_per_s", 10.0));
  EXPECT_TRUE(good.successful) << good.reason;
  EXPECT_DOUBLE_EQ(
    bridge.node().get_parameter("resend_giveup_warn_rate_per_s").as_double(),
    10.0);
}

// Rejecting names the node does not recognise is bad practice (rclcpp
// says so explicitly) — another part of the node may own them.
TEST(RuntimeTunables, UnrelatedParameterUntouched)
{
  Bridge bridge("runtime_unrelated");
  auto connection = bridge.configure();
  ASSERT_TRUE(connection != nullptr);

  bridge.node().declare_parameter("some_other_parameter", 3);
  auto result = bridge.node().set_parameter(
    rclcpp::Parameter("some_other_parameter", 4));
  EXPECT_TRUE(result.successful) << result.reason;
  EXPECT_FLOAT_EQ(connection->admissionFloorBytesPerSecond(),
                  static_cast<float>(kConfiguredFloor));
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

// End-to-end QoS-matching tests. The load-bearing claim of the package's
// QoS design (see udp_bridge/doc/qos_design.md) is that a publisher
// declared with BEST_AVAILABLE reliability matches subscribers that
// declare either RELIABLE or BEST_EFFORT — that's the mitigation that
// keeps a flat BEST_EFFORT default from silently breaking matching with
// downstream consumers like CAMP. This test exercises that assumption
// against the live rmw layer (whatever rmw is in use at test time).

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

#include "udp_bridge/qos_resolution.h"

using namespace std::chrono_literals;
using udp_bridge::resolveDestinationPublisherQos;

namespace
{

// Spin both nodes until either the predicate returns true or the deadline
// elapses. Returns whether the predicate became true.
template <typename Pred>
bool spin_until(rclcpp::executors::SingleThreadedExecutor& executor,
                Pred&& pred,
                std::chrono::milliseconds deadline = 2000ms)
{
  auto stop = std::chrono::steady_clock::now() + deadline;
  while(std::chrono::steady_clock::now() < stop)
  {
    if(pred())
      return true;
    executor.spin_some(50ms);
  }
  return pred();
}

// Construct two nodes that share the test topic, with the publisher's QoS
// resolved through our package helper. Returns true if the subscriber
// receives a published message within the deadline.
bool publish_and_observe(const std::string& reliability,
                         const std::string& durability,
                         uint32_t depth,
                         const rclcpp::QoS& subscriber_qos)
{
  static int test_counter = 0;
  const int test_id = ++test_counter;
  // Per-test unique names so repeated create/destroy cycles don't hit
  // DDS graph collisions or delayed teardown that flake discovery.
  std::string topic = "/udp_bridge_qos_test_" + std::to_string(test_id);
  std::string pub_node_name = "qos_match_pub_" + std::to_string(test_id);
  std::string sub_node_name = "qos_match_sub_" + std::to_string(test_id);

  auto pub_node = std::make_shared<rclcpp::Node>(pub_node_name);
  auto sub_node = std::make_shared<rclcpp::Node>(sub_node_name);

  auto pub_qos = resolveDestinationPublisherQos(reliability, durability, depth);
  auto publisher = pub_node->create_publisher<std_msgs::msg::String>(topic, pub_qos);

  std::atomic<bool> received{false};
  auto subscription = sub_node->create_subscription<std_msgs::msg::String>(
    topic, subscriber_qos,
    [&received](std_msgs::msg::String::SharedPtr) { received = true; });

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(pub_node);
  executor.add_node(sub_node);

  // Wait for discovery / matching to settle.
  spin_until(executor,
             [&]() { return publisher->get_subscription_count() > 0; },
             1500ms);

  // If the publisher has no matched subscriber after the deadline, the QoS
  // policies were incompatible. Report that as a non-match outcome.
  if(publisher->get_subscription_count() == 0)
    return false;

  std_msgs::msg::String message;
  message.data = "hello";
  publisher->publish(message);

  return spin_until(executor,
                    [&]() { return received.load(); },
                    1500ms);
}

} // namespace

class QosMatchingIntegration : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
  }
};

// --- The headline cases -----------------------------------------------------

TEST_F(QosMatchingIntegration, BestAvailablePublisher_MatchesReliableSubscriber)
{
  // The CAMP mitigation: subscribers with default rclcpp::QoS(N) declare
  // RELIABLE; the bridge's BEST_AVAILABLE publisher must match them so
  // operator-side topics keep flowing.
  rclcpp::QoS sub_qos(1);
  sub_qos.reliable();
  EXPECT_TRUE(publish_and_observe("best_available", "", 1, sub_qos))
    << "BEST_AVAILABLE publisher failed to match a RELIABLE subscriber. "
       "This is the load-bearing assumption of qos_design.md; if it fails, "
       "the package's QoS contract is broken under this rmw.";
}

TEST_F(QosMatchingIntegration, BestAvailablePublisher_MatchesBestEffortSubscriber)
{
  rclcpp::QoS sub_qos(1);
  sub_qos.best_effort();
  EXPECT_TRUE(publish_and_observe("best_available", "", 1, sub_qos))
    << "BEST_AVAILABLE publisher failed to match a BEST_EFFORT subscriber.";
}

// --- Sanity: explicit policies still work ---------------------------------

TEST_F(QosMatchingIntegration, ReliablePublisher_MatchesReliableSubscriber)
{
  rclcpp::QoS sub_qos(1);
  sub_qos.reliable();
  EXPECT_TRUE(publish_and_observe("reliable", "", 1, sub_qos));
}

TEST_F(QosMatchingIntegration, BestEffortPublisher_MatchesBestEffortSubscriber)
{
  rclcpp::QoS sub_qos(1);
  sub_qos.best_effort();
  EXPECT_TRUE(publish_and_observe("best_effort", "", 1, sub_qos));
}

// --- Negative case: confirms the matching mechanism exists ----------------
// Without this the all-pass result above could be a false positive
// (any-to-any). A BEST_EFFORT publisher must NOT match a RELIABLE subscriber.

TEST_F(QosMatchingIntegration, BestEffortPublisher_DoesNotMatchReliableSubscriber)
{
  rclcpp::QoS sub_qos(1);
  sub_qos.reliable();
  EXPECT_FALSE(publish_and_observe("best_effort", "", 1, sub_qos))
    << "BEST_EFFORT publisher unexpectedly matched a RELIABLE subscriber. "
       "If this is now allowed under the active rmw, the rest of qos_design.md "
       "needs revisiting.";
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  if(rclcpp::ok())
    rclcpp::shutdown();
  return rc;
}

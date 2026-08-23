// Failure-isolation tests for the relay fan-out (issue #51).
//
// udp_bridge::sendToEachDestination is the loop relayToOtherRemotes runs over
// its selected destinations. Two contract points, both of which the single
// try/catch it replaced got wrong:
//
//   ConnectionExceptionIsCaught  — Connection::send() throws
//                                 ConnectionException on the ordinary
//                                 Timeout path, and ConnectionException has
//                                 NO base class, so catch(std::exception&)
//                                 misses it. If it escapes, RelayQueue's
//                                 last-resort catch(...) swallows it with no
//                                 logger and every relay send failure is
//                                 silent.
//   OneFailingDestinationDoesNotCancelTheOthers — a stalled remote must not
//                                 abort the hub's fan-out to the remotes
//                                 that are still healthy.
//
// udp_bridge::relayToEachDestination is that loop WITH the per-destination
// rewrite, which is the composition the sink actually runs:
//
//   EachDestinationGetsItsOwnTopicAndQos — the rewrite happens once per
//                                 recipient, inside the loop. Hoisting it
//                                 above the loop would send every remote
//                                 whichever config was applied last — a
//                                 silent fan-out bug, since each message is
//                                 still well-formed — and previously no
//                                 test would have caught it: the rewrite
//                                 was tested standalone and the fan-out
//                                 was tested with stub sinks.
//   RewriteStillHappensAfterAFailedDestination — the isolation and the
//                                 rewrite compose: a throwing remote does
//                                 not leave the next one holding its
//                                 destination topic.

#include "udp_bridge/relay_send.h"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using udp_bridge::ConnectionException;
using udp_bridge::SelectedConnections;
using udp_bridge::DestinationConfig;
using udp_bridge::relayToEachDestination;
using udp_bridge::sendToEachDestination;

namespace
{

SelectedConnections threeDestinations()
{
  SelectedConnections destinations;
  destinations["operator_a"] = {"wifi"};
  destinations["operator_b"] = {"wan"};
  destinations["operator_c"] = {"wifi", "wan"};
  return destinations;
}

} // namespace

TEST(RelaySend, ConnectionExceptionIsCaught)
{
  SelectedConnections destinations;
  destinations["operator_a"] = {"wifi"};

  std::vector<std::string> errors;
  // No throw escapes: gtest would report the ConnectionException as an
  // unexpected exception (and in production it would reach RelayQueue's
  // logger-less catch(...)).
  sendToEachDestination(destinations,
    [](const std::string&, const std::vector<std::string>&)
    {
      throw ConnectionException("Timeout");
    },
    [&](const std::string& remote, const std::string& what)
    {
      errors.push_back(remote + ": " + what);
    });

  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors.front(), "operator_a: Timeout");
}

TEST(RelaySend, StdExceptionAndUnknownThrowsAreAlsoReported)
{
  SelectedConnections destinations;
  destinations["a"] = {"wifi"};
  destinations["b"] = {"wifi"};

  std::vector<std::string> errors;
  sendToEachDestination(destinations,
    [](const std::string& remote, const std::vector<std::string>&)
    {
      if(remote == "a")
        throw std::runtime_error("boom");
      throw 42;  // not derived from std::exception
    },
    [&](const std::string& remote, const std::string& what)
    {
      errors.push_back(remote + ": " + what);
    });

  ASSERT_EQ(errors.size(), 2u);
  EXPECT_EQ(errors[0], "a: boom");
  EXPECT_EQ(errors[1], "b: unknown exception");
}

TEST(RelaySend, OneFailingDestinationDoesNotCancelTheOthers)
{
  std::vector<std::string> sent;
  std::vector<std::string> errors;

  sendToEachDestination(threeDestinations(),
    [&](const std::string& remote, const std::vector<std::string>& connections)
    {
      if(remote == "operator_a")
        throw ConnectionException("Timeout");
      for(const auto& connection: connections)
        sent.push_back(remote + "/" + connection);
    },
    [&](const std::string& remote, const std::string& what)
    {
      errors.push_back(remote + ": " + what);
    });

  EXPECT_EQ(sent, (std::vector<std::string>{"operator_b/wan",
                                            "operator_c/wifi",
                                            "operator_c/wan"}))
    << "a stalled remote must not abort the relay to the healthy ones";
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors.front(), "operator_a: Timeout");
}

TEST(RelaySend, EveryDestinationIsSentToWhenNothingThrows)
{
  std::vector<std::string> sent;
  sendToEachDestination(threeDestinations(),
    [&](const std::string& remote, const std::vector<std::string>&)
    {
      sent.push_back(remote);
    },
    [](const std::string&, const std::string&)
    {
      FAIL() << "no destination should have reported an error";
    });

  EXPECT_EQ(sent, (std::vector<std::string>{"operator_a", "operator_b", "operator_c"}));
}

namespace
{

udp_bridge_interfaces::msg::MessageInternal relayedMessage()
{
  udp_bridge_interfaces::msg::MessageInternal message;
  message.source_topic = "/nav";              // the upstream publisher's name
  message.destination_topic = "/boat_a/nav";  // what the hub received it as
  message.datatype = "nav_msgs/msg/Odometry";
  message.data = {1, 2, 3, 4};
  return message;
}

std::map<std::string, DestinationConfig> twoDifferentConfigs()
{
  std::map<std::string, DestinationConfig> configs;
  DestinationConfig a;
  a.destination_topic = "/console/boat_a/nav";
  a.reliability = "reliable";
  a.durability = "volatile";
  a.history_depth = 5;
  configs["operator_a"] = a;
  DestinationConfig b;
  b.destination_topic = "/tablet/nav";
  b.reliability = "best_effort";
  b.durability = "transient_local";
  b.history_depth = 1;
  configs["operator_b"] = b;
  return configs;
}

struct Recorded
{
  std::string remote;
  std::string source_topic;
  std::string destination_topic;
  std::string reliability;
  uint32_t history_depth = 0;
  size_t payload_size = 0;
};

} // namespace

// Every recipient must see ITS OWN destination topic and QoS. If the
// rewrite were hoisted out of the fan-out loop, all recorded rows would
// carry one remote's config; the payload must be untouched throughout.
TEST(RelaySend, EachDestinationGetsItsOwnTopicAndQos)
{
  auto message = relayedMessage();
  const auto configs = twoDifferentConfigs();

  SelectedConnections destinations;
  destinations["operator_a"] = {"wifi"};
  destinations["operator_b"] = {"wan"};

  std::vector<Recorded> recorded;
  relayToEachDestination(message, "/boat_a/nav", destinations, configs,
    [&](const std::string& remote, const std::vector<std::string>&,
        udp_bridge_interfaces::msg::MessageInternal& rewritten)
    {
      recorded.push_back({remote, rewritten.source_topic,
                          rewritten.destination_topic, rewritten.reliability,
                          rewritten.history_depth, rewritten.data.size()});
    },
    [](const std::string&, const std::string&)
    {
      FAIL() << "no destination should have thrown";
    });

  ASSERT_EQ(recorded.size(), 2u);
  EXPECT_EQ(recorded[0].remote, "operator_a");
  EXPECT_EQ(recorded[0].destination_topic, "/console/boat_a/nav");
  EXPECT_EQ(recorded[0].reliability, "reliable");
  EXPECT_EQ(recorded[0].history_depth, 5u);
  EXPECT_EQ(recorded[1].remote, "operator_b");
  EXPECT_EQ(recorded[1].destination_topic, "/tablet/nav");
  EXPECT_EQ(recorded[1].reliability, "best_effort");
  EXPECT_EQ(recorded[1].history_depth, 1u);

  EXPECT_NE(recorded[0].destination_topic, recorded[1].destination_topic)
    << "a hoisted rewrite would give both remotes the same destination";

  for(const auto& row: recorded)
  {
    EXPECT_EQ(row.source_topic, "/boat_a/nav")
      << "source_topic is rewritten to the hub-local topic for every recipient";
    EXPECT_EQ(row.payload_size, 4u) << "the payload is never re-serialized";
  }
}

// A remote absent from the config map is sent a default-configured message
// rather than inheriting the previous remote's rewrite.
TEST(RelaySend, UnconfiguredDestinationGetsDefaultsNotTheLastRemotesConfig)
{
  auto message = relayedMessage();
  const auto configs = twoDifferentConfigs();

  SelectedConnections destinations;
  destinations["operator_a"] = {"wifi"};
  destinations["zz_unknown"] = {"wan"};  // sorts after operator_a

  std::vector<Recorded> recorded;
  relayToEachDestination(message, "/boat_a/nav", destinations, configs,
    [&](const std::string& remote, const std::vector<std::string>&,
        udp_bridge_interfaces::msg::MessageInternal& rewritten)
    {
      recorded.push_back({remote, rewritten.source_topic,
                          rewritten.destination_topic, rewritten.reliability,
                          rewritten.history_depth, rewritten.data.size()});
    },
    [](const std::string&, const std::string&) {});

  ASSERT_EQ(recorded.size(), 2u);
  EXPECT_EQ(recorded[1].remote, "zz_unknown");
  EXPECT_TRUE(recorded[1].destination_topic.empty())
    << "an unconfigured remote must not inherit operator_a's destination";
  EXPECT_TRUE(recorded[1].reliability.empty());
  EXPECT_EQ(recorded[1].history_depth, 0u);
}

// Failure isolation and the rewrite compose: the destination after a
// throwing one still gets its own config applied.
TEST(RelaySend, RewriteStillHappensAfterAFailedDestination)
{
  auto message = relayedMessage();
  const auto configs = twoDifferentConfigs();

  SelectedConnections destinations;
  destinations["operator_a"] = {"wifi"};
  destinations["operator_b"] = {"wan"};

  std::vector<std::string> errors;
  std::string last_destination_topic;
  relayToEachDestination(message, "/boat_a/nav", destinations, configs,
    [&](const std::string& remote, const std::vector<std::string>&,
        udp_bridge_interfaces::msg::MessageInternal& rewritten)
    {
      if(remote == "operator_a")
        throw ConnectionException("Timeout");
      last_destination_topic = rewritten.destination_topic;
    },
    [&](const std::string& remote, const std::string& what)
    {
      errors.push_back(remote + ": " + what);
    });

  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors.front(), "operator_a: Timeout");
  EXPECT_EQ(last_destination_topic, "/tablet/nav");
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

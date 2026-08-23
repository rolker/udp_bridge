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

#include "udp_bridge/relay_send.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using udp_bridge::ConnectionException;
using udp_bridge::SelectedConnections;
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

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

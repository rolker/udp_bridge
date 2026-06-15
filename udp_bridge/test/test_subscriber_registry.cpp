// Unit tests for removeSubscriberConnectionFrom — the registry map logic behind
// the remove_subscribe / remove_advertise services (issue #30). Pure map logic,
// so it runs without bringing up a node: entries carry a null subscription and
// we assert the connection/remote/source erase cascade + idempotency.

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "udp_bridge/subscriber_registry.h"
#include "udp_bridge/types.h"

namespace
{

using udp_bridge::removeSubscriberConnectionFrom;
using udp_bridge::SubscriberDetails;

// Add a (source -> remote -> connection) forwarding entry (null subscription).
void addConn(std::map<std::string, SubscriberDetails>& subs,
             const std::string& source, const std::string& remote,
             const std::string& conn)
{
  subs[source].remote_details[remote].connection_rates[conn].period = 0.0f;
}

TEST(SubscriberRegistry, RemoveLastConnectionErasesSource)
{
  std::map<std::string, SubscriberDetails> subs;
  addConn(subs, "/state", "boat", "default");

  auto r = removeSubscriberConnectionFrom(subs, "/state", "boat", "default");
  EXPECT_TRUE(r.changed);
  EXPECT_EQ(subs.count("/state"), 0u);  // last remote gone -> source entry erased
}

TEST(SubscriberRegistry, RemoveOneConnectionKeepsRemote)
{
  std::map<std::string, SubscriberDetails> subs;
  addConn(subs, "/state", "boat", "wifi");
  addConn(subs, "/state", "boat", "vpn");

  auto r = removeSubscriberConnectionFrom(subs, "/state", "boat", "wifi");
  EXPECT_TRUE(r.changed);
  ASSERT_EQ(subs.count("/state"), 1u);
  auto& rd = subs["/state"].remote_details.at("boat");
  EXPECT_EQ(rd.connection_rates.count("wifi"), 0u);
  EXPECT_EQ(rd.connection_rates.count("vpn"), 1u);
}

TEST(SubscriberRegistry, RemoveOneRemoteKeepsOthers)
{
  std::map<std::string, SubscriberDetails> subs;
  addConn(subs, "/state", "boatA", "default");
  addConn(subs, "/state", "boatB", "default");

  auto r = removeSubscriberConnectionFrom(subs, "/state", "boatA", "default");
  EXPECT_TRUE(r.changed);
  ASSERT_EQ(subs.count("/state"), 1u);
  EXPECT_EQ(subs["/state"].remote_details.count("boatA"), 0u);
  EXPECT_EQ(subs["/state"].remote_details.count("boatB"), 1u);
}

TEST(SubscriberRegistry, RemoveAbsentSourceIsNoop)
{
  std::map<std::string, SubscriberDetails> subs;
  addConn(subs, "/state", "boat", "default");

  auto r = removeSubscriberConnectionFrom(subs, "/other", "boat", "default");
  EXPECT_FALSE(r.changed);
  EXPECT_EQ(subs.count("/state"), 1u);
}

TEST(SubscriberRegistry, RemoveAbsentRemoteIsNoop)
{
  std::map<std::string, SubscriberDetails> subs;
  addConn(subs, "/state", "boat", "default");

  auto r = removeSubscriberConnectionFrom(subs, "/state", "ghost", "default");
  EXPECT_FALSE(r.changed);
  EXPECT_EQ(subs["/state"].remote_details.count("boat"), 1u);
}

TEST(SubscriberRegistry, RemoveAbsentConnectionIsNoop)
{
  std::map<std::string, SubscriberDetails> subs;
  addConn(subs, "/state", "boat", "default");

  auto r = removeSubscriberConnectionFrom(subs, "/state", "boat", "ghost");
  EXPECT_FALSE(r.changed);
  EXPECT_EQ(subs["/state"].remote_details.at("boat").connection_rates.count("default"), 1u);
}

TEST(SubscriberRegistry, SecondRemoveIsIdempotentNoop)
{
  std::map<std::string, SubscriberDetails> subs;
  addConn(subs, "/state", "boat", "default");

  auto first = removeSubscriberConnectionFrom(subs, "/state", "boat", "default");
  EXPECT_TRUE(first.changed);
  auto second = removeSubscriberConnectionFrom(subs, "/state", "boat", "default");
  EXPECT_FALSE(second.changed);
  EXPECT_EQ(subs.count("/state"), 0u);
}

}  // namespace

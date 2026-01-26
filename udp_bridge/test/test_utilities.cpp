#include "udp_bridge/utilities.h"
#include <gtest/gtest.h>

TEST(UDPBridge_Utilities, topicStringTest)
{
    // Default behavior: keep_slashes = false, legal_first = 'r', legal_char = '_'
    EXPECT_EQ(udp_bridge::topicString("test_topic"), "test_topic");
    EXPECT_EQ(udp_bridge::topicString("/test/topic"), "r_test_topic");
    
    // Explicitly keeping slashes
    EXPECT_EQ(udp_bridge::topicString("/test/topic", true), "/test/topic");
    
    // Replacement of illegal characters
    EXPECT_EQ(udp_bridge::topicString("test-topic", true, 'a', '_'), "test_topic");
    
    // Removing slashes explicitly
    EXPECT_EQ(udp_bridge::topicString("test/topic", false, 'a', '_'), "test_topic");

    // Prepending legal_first if it doesn't start with alpha or /
    EXPECT_EQ(udp_bridge::topicString("123topic", true, 'a', '_'), "a123topic");
}

int main(int argc, char **argv){
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

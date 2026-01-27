#include "udp_bridge/defragmenter.h"
#include "udp_bridge/packet.h"
#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>

TEST(UDPBridge_Defragmenter, fragmentationTest)
{
    udp_bridge::Defragmenter defrag;
    
    std::vector<uint8_t> original_data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint32_t packet_id = 123;
    
    // Fragment 1 of 2
    std::vector<uint8_t> f1_data(sizeof(udp_bridge::FragmentHeader) + 5);
    udp_bridge::Fragment* f1 = reinterpret_cast<udp_bridge::Fragment*>(f1_data.data());
    f1->type = udp_bridge::PacketType::Fragment;
    f1->packet_id = packet_id;
    f1->fragment_number = 0;
    f1->fragment_count = 2;
    std::copy(original_data.begin(), original_data.begin() + 5, f1->fragment_data);
    
    // Fragment 2 of 2
    std::vector<uint8_t> f2_data(sizeof(udp_bridge::FragmentHeader) + 5);
    udp_bridge::Fragment* f2 = reinterpret_cast<udp_bridge::Fragment*>(f2_data.data());
    f2->type = udp_bridge::PacketType::Fragment;
    f2->packet_id = packet_id;
    f2->fragment_number = 1;
    f2->fragment_count = 2;
    std::copy(original_data.begin() + 5, original_data.end(), f2->fragment_data);
    
    rclcpp::Time now = rclcpp::Clock().now();
    
    EXPECT_FALSE(defrag.addFragment(f1_data, now));
    EXPECT_TRUE(defrag.addFragment(f2_data, now));
    
    auto packets = defrag.getPackets();
    EXPECT_EQ(packets.size(), 1);
    EXPECT_EQ(packets[0], original_data);
}

TEST(UDPBridge_Defragmenter, cleanupTest)
{
    udp_bridge::Defragmenter defrag;
    
    std::vector<uint8_t> f1_data(sizeof(udp_bridge::FragmentHeader) + 5);
    udp_bridge::Fragment* f1 = reinterpret_cast<udp_bridge::Fragment*>(f1_data.data());
    f1->type = udp_bridge::PacketType::Fragment;
    f1->packet_id = 1;
    f1->fragment_number = 0;
    f1->fragment_count = 2;
    
    rclcpp::Time t1(1, 0, RCL_ROS_TIME);
    defrag.addFragment(f1_data, t1);
    
    // Should NOT cleanup if discard_time is before arrival
    EXPECT_EQ(defrag.cleanup(rclcpp::Time(0, 500000000, RCL_ROS_TIME)), 0);
    
    // Should cleanup if discard_time is after arrival
    EXPECT_EQ(defrag.cleanup(rclcpp::Time(1, 100000000, RCL_ROS_TIME)), 1);
}

int main(int argc, char **argv){
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

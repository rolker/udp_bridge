#include "udp_bridge/packet.h"
#include <gtest/gtest.h>
#include "ros/ros.h"

TEST(UDPBridge_Packet, compressionTest)
{
    std::string test_in {"foobar foobar foobar foobar"};
    std::vector<uint8_t> data_in(test_in.begin(),test_in.end());
    auto compressed_data = udp_bridge::compress(data_in);
    
    udp_bridge::CompressedPacket *compressed_packet = reinterpret_cast<udp_bridge::CompressedPacket*>(compressed_data.data());
    EXPECT_EQ(compressed_packet->type, udp_bridge::PacketType::Compressed);
    EXPECT_LE(compressed_packet->uncompressed_size, test_in.size());
    
    EXPECT_LT(compressed_data.size(), data_in.size());
    
    auto decompressed_data = udp_bridge::uncompress(compressed_data);
    EXPECT_EQ(data_in.size(), decompressed_data.size());
    EXPECT_EQ(data_in, decompressed_data);
    
    std::string decompressed_string(decompressed_data.begin(), decompressed_data.end());
    EXPECT_EQ(test_in, decompressed_string);
}

TEST(UDPBridge_Packet, addressToDottedTest)
{
    sockaddr_in address {}; 
    address.sin_addr.s_addr = htonl(0x7f000001);
    
    EXPECT_EQ(udp_bridge::addressToDotted(address),"127.0.0.1");
}


int main(int argc, char **argv){
  testing::InitGoogleTest(&argc, argv);
  ros::init(argc, argv, "udp_bridge_tester");
  ros::NodeHandle nh;
  return RUN_ALL_TESTS();
}

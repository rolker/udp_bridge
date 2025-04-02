#ifndef UDP_BRIDGE_WRAPPED_PACKET_H
#define UDP_BRIDGE_WRAPPED_PACKET_H

#include "udp_bridge/packet.h"
#include "rclcpp/time.hpp"

namespace udp_bridge
{

/// Generates a SequencedPacket that can be sent using UDP.
struct WrappedPacket: SequencedPacketHeader
{
  WrappedPacket() = default;
  WrappedPacket(uint64_t packet_number, const std::vector<uint8_t>& data, rclcpp::Time now);
  WrappedPacket(const WrappedPacket& other, std::string source_node, std::string connection_id);
  std::vector<uint8_t> packet;
  rclcpp::Time timestamp;        
};

} // namespace udp_bridge

#endif

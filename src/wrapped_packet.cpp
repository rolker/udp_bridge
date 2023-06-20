#include "udp_bridge/wrapped_packet.h"

namespace udp_bridge
{

WrappedPacket::WrappedPacket(uint64_t packet_num, const std::vector<uint8_t>& data)
{
  memset(this, 0, sizeof(SequencedPacketHeader));
  packet_number = packet_num;
  packet_size = sizeof(SequencedPacketHeader)+data.size();
  timestamp = ros::Time::now();
  type = PacketType::WrappedPacket;
  packet.resize(packet_size);
  memcpy(packet.data(), this, sizeof(SequencedPacketHeader));
  memcpy(reinterpret_cast<SequencedPacket*>(packet.data())->packet, data.data(), data.size());
}
  
WrappedPacket::WrappedPacket(const WrappedPacket& other, std::string src_node, std::string cid)
{
  type = PacketType::WrappedPacket;
  packet_number = other.packet_number;
  packet_size = other.packet_size;
  timestamp = other.timestamp;
  packet = other.packet;

  memset(&source_node, 0, maximum_node_name_size);
  memcpy(&source_node, src_node.c_str(), std::min(src_node.size(), std::size_t(maximum_node_name_size-1)));

  memset(&channel_id, 0, maximum_channel_id_size);
  memcpy(&channel_id, cid.c_str(), std::min(cid.size(), std::size_t(maximum_channel_id_size-1)));

  memcpy(packet.data(), this, sizeof(SequencedPacketHeader));
}


} // namespace udp_bridge

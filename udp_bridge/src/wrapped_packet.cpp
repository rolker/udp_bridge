#include "udp_bridge/wrapped_packet.h"
#include <cstring>

namespace udp_bridge
{


WrappedPacket::WrappedPacket(uint64_t packet_num, const std::vector<uint8_t>& data, rclcpp::Time now)
{
  memset(this, 0, sizeof(SequencedPacketHeader));
  packet_number = packet_num;
  packet_size = sizeof(SequencedPacketHeader)+data.size();
  timestamp = now;
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

  // Last line of defence, not a policy (issue #51). Every path a node name
  // enters the bridge by now REJECTS an over-long name — `setName` fails
  // on_configure, `resolveRemoteIdentities` fails on_configure, the
  // add_remote service refuses — so a validated configuration can no
  // longer reach this clamp. It stays because this is the one place that
  // writes the fixed-size wire field, and a bound here is what makes the
  // write provably safe whatever a future caller does; it must never
  // become the place a length problem is silently resolved.
  memset(&source_node, 0, maximum_node_name_size);
  memcpy(&source_node, src_node.c_str(), std::min(src_node.size(), std::size_t(maximum_node_name_size-1)));

  memset(&connection_id, 0, maximum_connection_id_size);
  memcpy(&connection_id, cid.c_str(), std::min(cid.size(), std::size_t(maximum_connection_id_size-1)));

  memcpy(packet.data(), this, sizeof(SequencedPacketHeader));
}


} // namespace udp_bridge

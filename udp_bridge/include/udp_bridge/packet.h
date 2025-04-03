#ifndef UDP_BRIDGE_PACKET_H
#define UDP_BRIDGE_PACKET_H

#include <memory>
#include <cstdint>
#include <vector>
#include <string>


/// \file packet.h
/// Defines the structures used for sending data as UDP packets.
/// In addition to the structs defined here, ROS message types
/// are also used.
/// \defgroup packet UDP Packets

namespace udp_bridge
{

constexpr uint8_t maximum_node_name_size = 24;
constexpr uint8_t maximum_connection_id_size = 8;

/// Packet type identifiers.
/// \ingroup packet
enum class PacketType: uint8_t {
  Data,              ///< packet containing MessageInternal message containing a message from a transmitted topic
  Compressed,        ///< packet of type CompressedPacket
  SubscribeRequest,  ///< packet containing a RemoteSubscribeInternal message
  Fragment,          ///< packet of type Fragment
  BridgeInfo,        ///< packet containing a BridgeInfo message
  TopicStatistics,   ///< packet containing a TopicStatistics message
  WrappedPacket,     ///< packet of type SequencedPacket
  ResendRequest,     ///< packet containing a ResendRequest message
  Connection,        ///< packet of type ConnectionInternal
};

template<typename MessageType> PacketType packetTypeOf(const MessageType&);

#pragma pack(push, 1)

struct PacketHeader
{
  PacketType type;
};

/// Represents any supported packet.
/// \ingroup packet
struct Packet: public PacketHeader
{
  uint8_t data[];
};

struct CompressedPacketHeader: public PacketHeader
{
  /// Size needed for the uncompressed version compressed_data.
  uint32_t uncompressed_size;
};

/// Packet containing a compressed packet.
/// \ingroup packet
struct CompressedPacket: public CompressedPacketHeader
{
  uint8_t compressed_data[];
};

struct FragmentHeader: public PacketHeader
{
  uint32_t packet_id;
  uint16_t fragment_number;
  uint16_t fragment_count;
};

/// Packet containing a fragment of a larger packet.
/// \ingroup packet
struct Fragment: public FragmentHeader
{
  uint8_t fragment_data[];
};

struct SequencedPacketHeader: public PacketHeader
{
  char source_node[maximum_node_name_size];
  char connection_id[maximum_connection_id_size];
  uint64_t packet_number;
  uint32_t packet_size;
};

/// Wrapper packet containing a packet as well as connection metadata
/// \ingroup packet
struct SequencedPacket: public SequencedPacketHeader
{
  uint8_t packet[];
};

#pragma pack(pop)

std::vector<uint8_t> compress(const std::vector<uint8_t> &data);
std::vector<uint8_t> uncompress(std::vector<uint8_t> const &data);

} // namespace udp_bridge

#endif

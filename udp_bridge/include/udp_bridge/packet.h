#ifndef UDP_BRIDGE_PACKET_H
#define UDP_BRIDGE_PACKET_H

#include <memory>
#include <cstdint>
#include <cstring>
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

// Canonical wire form of a connection id: the leading
// `maximum_connection_id_size - 1` bytes of the input (the trailing
// byte of the on-wire char array is the null terminator).
//
// Centralizing the truncation here keeps the receive-side wire clamp
// (UDPBridge::decodeResendRequest), the sender-side stamp
// (UDPBridge::resendMissingPackets), and the connection-map insertion
// points (RemoteNode::newConnection / connection,
// Connection::Connection) using the same canonical form. The
// connections_ map is keyed on this canonical form so that a
// ResendRequest stamped with the truncated wire id always finds the
// connection that holds sent_packets_ for that link, even when the
// configured id is longer than the on-wire field — closes the
// truncation-contract gap that bit issue #23's round-8 self-review.
inline std::string truncate_connection_id(const std::string& id)
{
  if(id.size() < maximum_connection_id_size)
    return id;
  return id.substr(0, maximum_connection_id_size - 1);
}

// Longest node name representable on the wire. `source_node` is a fixed
// `char[maximum_node_name_size]` whose trailing byte is the null
// terminator, so a name of this length is the largest that survives a
// round trip unchanged.
//
// This is a HARD limit, not a truncation point (issue #51). Truncating a
// configured name is what manufactures a mismatch: the local name and the
// remote's configured identity for it are supposed to be the same string,
// and silently shortening one of them makes them unequal, which puts the
// relay loop rule to sleep and makes the hub echo. Truncation also lets two
// configured names that differ only after this many characters collapse to
// one wire identity, which is precisely the collision
// `resolveRemoteIdentities` exists to reject. Every path a name enters the
// bridge by therefore REJECTS an over-long name loudly (on_configure fails,
// the add_remote service refuses) rather than shortening it, so the
// remaining truncation in `WrappedPacket`'s constructor is a last-line-of-
// defence bound that validated configuration can no longer reach.
constexpr std::size_t maximum_node_name_length = maximum_node_name_size - 1;

// True when `name` fits the on-wire node-name field.
inline bool node_name_fits(const std::string& name)
{
  return name.size() <= maximum_node_name_length;
}

// Human-readable rejection for an over-long node name: names the parameter
// (or service field) it came from, the offending value, its length and the
// limit, so the operator can fix the config without reading the source.
inline std::string node_name_too_long_error(const std::string& source,
                                            const std::string& name)
{
  return source + " '" + name + "' is " + std::to_string(name.size())
       + " characters; the maximum is "
       + std::to_string(maximum_node_name_length)
       + " (the on-wire node-name field is "
       + std::to_string(int(maximum_node_name_size))
       + " bytes including the null terminator). Shorten the name — it is"
         " NOT truncated, because a truncated name no longer matches the"
         " name the other bridge is configured with.";
}

// Read a fixed-size on-wire character field as a std::string without
// running off the end of it. The fields are null-padded by our own write
// side (WrappedPacket memsets before copying), but a corrupted or foreign
// packet can fill all `size` bytes with non-zero data, and constructing a
// std::string from the bare pointer would then read past the field — and
// past the allocation, if no null byte happens to follow. Bound the scan.
inline std::string wire_field_to_string(const char* field, std::size_t size)
{
  std::size_t length = 0;
  while(length < size && field[length] != '\0')
    ++length;
  return std::string(field, length);
}

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

#ifndef UDP_BRIDGE_SOCKET_H
#define UDP_BRIDGE_SOCKET_H

#include <memory>
#include <cstdint>
#include <vector>
#include <string>
#include <netinet/in.h>

namespace udp_bridge
{

enum class PacketType: uint8_t {Data, Compressed, SubscribeRequest, AdvertiseRequest, ChannelInfo, Fragment, BridgeInfo};

#pragma pack(push, 1)

struct PacketHeader
{
    PacketType type;
};

struct Packet: public PacketHeader
{
    uint8_t data[];
};

struct CompressedPacketHeader: public PacketHeader
{
    uint32_t uncompressed_size;
};

struct CompressedPacket: public CompressedPacketHeader
{
    uint8_t compressed_data[];
};

struct FragmentHeader: public PacketHeader
{
    uint32_t packet_id;
    uint8_t fragment_number;
    uint8_t fragment_count;
};

struct Fragment: public FragmentHeader
{
    uint8_t fragment_data[];
};

#pragma pack(pop)

std::shared_ptr<std::vector<uint8_t> > compress(std::vector<uint8_t> const &data);
std::vector<uint8_t> uncompress(std::vector<uint8_t> const &data);

std::string addressToDotted(const sockaddr_in &address);


} // namespace udp_bridge

#endif

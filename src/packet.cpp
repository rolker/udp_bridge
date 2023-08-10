#include "udp_bridge/packet.h"
#include <zlib.h>
#include <iostream>

#include <udp_bridge/MessageInternal.h>
#include <udp_bridge/RemoteSubscribeInternal.h>
#include <udp_bridge/BridgeInfo.h>
#include <udp_bridge/TopicStatisticsArray.h>
#include <udp_bridge/ResendRequest.h>
#include <udp_bridge/ConnectionInternal.h>

namespace udp_bridge
{

template<> PacketType packetTypeOf(const MessageInternal&) { return PacketType::Data;}
template<> PacketType packetTypeOf(const RemoteSubscribeInternal&) { return PacketType::SubscribeRequest;}
template<> PacketType packetTypeOf(const BridgeInfo&) { return PacketType::BridgeInfo;}
template<> PacketType packetTypeOf(const TopicStatisticsArray&) { return PacketType::TopicStatistics;}
template<> PacketType packetTypeOf(const ResendRequest&) { return PacketType::ResendRequest;}
template<> PacketType packetTypeOf(const ConnectionInternal&) { return PacketType::Connection;}


std::vector<uint8_t> compress(const std::vector<uint8_t>& data)
{
  uLong compressed_buffer_size = compressBound(data.size());
  std::vector<uint8_t> compressed_buffer(sizeof(CompressedPacketHeader)+compressed_buffer_size);
  CompressedPacket * compressed_packet = reinterpret_cast<CompressedPacket*>(compressed_buffer.data());
  int compress_return = ::compress(compressed_packet->compressed_data, &compressed_buffer_size, data.data(), data.size());
  if(compress_return == Z_OK)
  {
    compressed_packet->type = PacketType::Compressed;
    compressed_packet->uncompressed_size = data.size();
    compressed_buffer.resize(sizeof(CompressedPacketHeader)+compressed_buffer_size);
    if(compressed_buffer.size() < data.size())
      return compressed_buffer;
  }
  return data;
}

std::vector<uint8_t> uncompress(std::vector<uint8_t> const &data)
{
    uLong comp_size = data.size()-sizeof(CompressedPacketHeader);
    const CompressedPacket *compressed_packet = reinterpret_cast<const CompressedPacket*>(data.data());
    uLong decomp_size = compressed_packet->uncompressed_size;
    std::vector<uint8_t> ret(decomp_size);
    int decomp_ret = ::uncompress(ret.data(),&decomp_size,compressed_packet->compressed_data,comp_size);
    if(decomp_ret != Z_OK)
    {
        std::cerr << "uncompress: comp size: " << comp_size << std::endl;
        std::cerr << "uncompress: decomp size in: " << compressed_packet->uncompressed_size << std::endl;
        std::cerr << "uncompress: decomp size out: " << decomp_size << std::endl;
        switch(decomp_ret)
        {
            case Z_MEM_ERROR:
                std::cerr << "uncompress: not enough memory" << std::endl;
                break;
            case Z_BUF_ERROR:
                std::cerr << "uncompress: not enough room in output buffer" << std::endl;
                break;
            case Z_DATA_ERROR:
                std::cerr << "uncompress: corrupt or incomplete input data" << std::endl;
                break;
        }
        ret.clear();
    }
    return ret;
}    
    
    
    
} // namespace udp_bridge

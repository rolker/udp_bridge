#include "udp_bridge/packet.h"
#include <zlib.h>
#include <iostream>

namespace udp_bridge
{

std::shared_ptr<std::vector<uint8_t> > compress(std::vector<uint8_t> const &data)
{
    uLong comp_buffer_size = compressBound(data.size());
    auto ret = std::make_shared<std::vector<uint8_t> >(sizeof(CompressedPacketHeader)+comp_buffer_size);
    CompressedPacket * compressed_packet = reinterpret_cast<CompressedPacket*>(ret->data());
    int com_ret = ::compress(compressed_packet->compressed_data,&comp_buffer_size, data.data(), data.size());
    if(com_ret == Z_OK)
    {
        compressed_packet->type = PacketType::Compressed;
        compressed_packet->uncompressed_size = data.size();
        ret->resize(sizeof(CompressedPacketHeader)+comp_buffer_size);
    }
    else
        ret.reset();
    return ret;
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
    
std::string addressToDotted(const sockaddr_in& address)
{
    return std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[0])+"."+
           std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[1])+"."+
           std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[2])+"."+
           std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[3]);
}
    
    
} // namespace udp_bridge


.. _program_listing_file_include_udp_bridge_packet.h:

Program Listing for File packet.h
=================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_packet.h>` (``include/udp_bridge/packet.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_PACKET_H
   #define UDP_BRIDGE_PACKET_H
   
   #include <memory>
   #include <cstdint>
   #include <vector>
   #include <string>
   
   
   
   namespace udp_bridge
   {
   
   constexpr uint8_t maximum_node_name_size = 24;
   constexpr uint8_t maximum_connection_id_size = 8;
   
   enum class PacketType: uint8_t {
     Data,              
     Compressed,        
     SubscribeRequest,  
     Fragment,          
     BridgeInfo,        
     TopicStatistics,   
     WrappedPacket,     
     ResendRequest,     
     Connection,        
   };
   
   template<typename MessageType> PacketType packetTypeOf(const MessageType&);
   
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
     uint16_t fragment_number;
     uint16_t fragment_count;
   };
   
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
   
   struct SequencedPacket: public SequencedPacketHeader
   {
     uint8_t packet[];
   };
   
   #pragma pack(pop)
   
   std::vector<uint8_t> compress(const std::vector<uint8_t> &data);
   std::vector<uint8_t> uncompress(std::vector<uint8_t> const &data);
   
   } // namespace udp_bridge
   
   #endif

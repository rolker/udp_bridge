
.. _program_listing_file_include_udp_bridge_wrapped_packet.h:

Program Listing for File wrapped_packet.h
=========================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_wrapped_packet.h>` (``include/udp_bridge/wrapped_packet.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_WRAPPED_PACKET_H
   #define UDP_BRIDGE_WRAPPED_PACKET_H
   
   #include "udp_bridge/packet.h"
   #include "rclcpp/time.hpp"
   
   namespace udp_bridge
   {
   
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

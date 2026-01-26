
.. _program_listing_file_include_udp_bridge_defragmenter.h:

Program Listing for File defragmenter.h
=======================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_defragmenter.h>` (``include/udp_bridge/defragmenter.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_DEFRAGMENTER_H
   #define UDP_BRIDGE_DEFRAGMENTER_H
   
   #include "rclcpp/rclcpp.hpp"
   #include "packet.h"
   #include <map>
   
   namespace udp_bridge
   {
   
   class Defragmenter
   {
     struct Fragments
     {
       uint16_t fragment_count;
       rclcpp::Time first_arrival_time;
       std::map<uint16_t, std::vector<uint8_t> > fragment_map;
     };
   public:
     bool addFragment(std::vector<uint8_t> fragment, rclcpp::Time now);
   
     std::vector<std::vector<uint8_t> > getPackets();
   
     int cleanup(rclcpp::Time discard_time);
   private:
     std::map<uint32_t, Fragments> fragment_map_;
     std::vector<std::vector<uint8_t> > complete_packets_;
   };
   
   } // namespace udp_bridge
   
   #endif


.. _program_listing_file_include_udp_bridge_types.h:

Program Listing for File types.h
================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_types.h>` (``include/udp_bridge/types.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_TYPES_H
   #define UDP_BRIDGE_TYPES_H
   
   #include "udp_bridge/statistics.h"
   #include "rclcpp/generic_subscription.hpp"
   
   namespace udp_bridge
   {
   
   struct ConnectionRateInfo
   {
     float period;
     rclcpp::Time last_sent_time;
   };
   
   struct RemoteDetails
   {
     RemoteDetails()=default;
     //RemoteDetails(std::string const &destination_topic, float period):destination_topic(destination_topic),period(period)
     //{}
         
     std::string destination_topic;
     std::map<std::string, ConnectionRateInfo> connection_rates;
   
   };
       
   struct SubscriberDetails
   {
     rclcpp::GenericSubscription::SharedPtr subscription;
     uint32_t queue_size;
     std::map<std::string, RemoteDetails> remote_details;
     MessageStatistics statistics;
   };
   
   // Name and address/port from which a message was received
   struct SourceInfo
   {
     std::string node_name;
     std::string host;
     uint16_t port = 0;
   };
   
   } // namespace udp_bridge
   
   #endif

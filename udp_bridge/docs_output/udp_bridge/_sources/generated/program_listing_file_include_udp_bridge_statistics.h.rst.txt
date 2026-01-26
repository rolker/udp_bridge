
.. _program_listing_file_include_udp_bridge_statistics.h:

Program Listing for File statistics.h
=====================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_statistics.h>` (``include/udp_bridge/statistics.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_STATISTICS_H
   #define UDP_BRIDGE_STATISTICS_H
   
   #include <deque>
   #include <map>
   #include "rclcpp/time.hpp"
   #include "udp_bridge_interfaces/msg/topic_statistics.hpp"
   #include "udp_bridge_interfaces/msg/data_rates.hpp"
   
   namespace udp_bridge
   {
   
   enum struct SendResult 
   {
     success,
     failed, 
     dropped, 
   };
   
   struct MessageSizeData
   {
     int message_size = 0;
   
     int fragment_count = 0;
   
     int sent_size = 0;
   
     rclcpp::Time timestamp;
     std::map<std::string, std::map<std::string, SendResult> > send_results;
   };
   
   
   enum struct PacketSendCategory
   {
     message,
     overhead,
     resend
   };
   
   struct PacketSizeData
   {
     rclcpp::Time timestamp;
     u_int16_t size;
     PacketSendCategory category;
     SendResult send_result;
   };
   
   template<typename T> class Statistics
   {
   public:
     void add(const T& data)
     {
       if(data.timestamp.nanoseconds() != 0)
         data_.push_back(data);
   
       // only keep 10 seconds of data
       while(!data_.empty() && data_.front().timestamp < data.timestamp - rclcpp::Duration::from_seconds(10.0))
         data_.pop_front();
     }
   protected:
     std::deque<T> data_;
   };
   
   class MessageStatistics: public Statistics<MessageSizeData>
   {
   public:
     void add(const MessageSizeData& data);
   
     std::vector<udp_bridge_interfaces::msg::TopicStatistics> get();
   };
   
   
   class PacketSendStatistics: public Statistics<PacketSizeData>
   {
   public:
     udp_bridge_interfaces::msg::DataRates get() const;
     udp_bridge_interfaces::msg::DataRates get(PacketSendCategory category) const;
   
     bool can_send(uint32_t data_size, uint32_t bytes_per_second_limit, rclcpp::Time time) const;
   
   private:
     udp_bridge_interfaces::msg::DataRates get(PacketSendCategory *category) const;
   
   };
   
   } // namespace udp_bridge
   
   #endif


.. _program_listing_file_include_udp_bridge_connection.h:

Program Listing for File connection.h
=====================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_connection.h>` (``include/udp_bridge/connection.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_CONNECTION_H
   #define UDP_BRIDGE_CONNECTION_H
   
   //#include <ros/ros.h>
   #include <vector>
   #include <cstdint>
   #include <string>
   #include <memory>
   #include <list>
   #include <map>
   #include <netinet/in.h>
   #include <udp_bridge/packet.h>
   #include "udp_bridge/types.h"
   #include "udp_bridge/wrapped_packet.h"
   #include "udp_bridge_interfaces/msg/data_rates.hpp"
   
   namespace udp_bridge
   {
   
   class ConnectionException
   {
     public:
       ConnectionException(const std::string &msg):msg_(msg) {}
       const std::string & getMessage() const {return msg_;}
     private:
       std::string msg_;
   };
   
   class Connection
   {
   public:
     Connection(std::string id, std::string const &host, uint16_t port, std::string return_host=std::string(), uint16_t return_port=0);
   
     void setHostAndPort(const std::string& host, uint16_t port);
     void setRateLimit(uint32_t maximum_bytes_per_second);
     uint32_t rateLimit() const;
   
     std::string str() const;
   
     const std::string &id() const;
   
     // Used to tell the remote host the address to get back to us.
     const std::string& returnHost() const;
     uint16_t returnPort() const;
     void setReturnHostAndPort(const std::string &return_host, uint16_t return_port);
   
     // IP address and port from incoming packets. May differ from where packets are sent depending
     // on network architecture.
     const std::string& sourceIPAddress() const;
     uint16_t sourcePort() const;
     void setSourceIPAndPort(const std::string &source_ip, uint16_t source_port);
   
   
     const std::string& host() const;
     uint16_t port() const;
     const std::string& ip_address() const;
     std::string ip_address_with_port() const;
   
     const sockaddr_in* socket_address() const;
   
     const double& last_receive_time() const;
     void update_last_receive_time(double t, int data_size, bool duplicate);
   
     void resend_packets(const std::vector<uint64_t> &missing_packets, int socket, rclcpp::Time now);
   
     SendResult send(const std::vector<WrappedPacket>& packets, int socket, const std::string& remote, bool is_overhead, rclcpp::Time now);
   
     PacketSizeData send(const std::vector<uint8_t> &data, int socket, PacketSendCategory category, rclcpp::Time now);
   
     //bool can_send(uint32_t byte_count, double time);
   
     std::pair<double, double>  data_receive_rate(double time);
   
     udp_bridge_interfaces::msg::DataRates data_sent_rate(rclcpp::Time time, PacketSendCategory category);
   
     void cleanup_sent_packets(rclcpp::Time cutoff_time);
   
   private:
     void resolveHost();
   
     std::string id_;
   
     std::string host_;
     std::string ip_address_; 
     uint16_t port_;
   
     std::vector<sockaddr_in> addresses_;
   
     std::string return_host_;
     uint16_t return_port_ = 0;
   
     std::string source_ip_address_; 
     uint16_t source_port_ = 0; 
   
     double last_receive_time_ = 0.0;
   
     static constexpr uint32_t default_rate_limit = 50000;
   
     uint32_t data_rate_limit_ = default_rate_limit;
   
     struct ReceivedSize
     {
       uint16_t size = 0;
   
       bool duplicate = false;
     };
     std::map<double, std::vector<ReceivedSize> > data_size_received_history_;
   
     std::map<uint64_t, WrappedPacket> sent_packets_;
   
     PacketSendStatistics sent_packet_statistics_;
   };
   
   
   std::string addressToDotted(const sockaddr_in &address);
       
   } // namespace udp_bridge
   
   #endif

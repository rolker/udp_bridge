
.. _program_listing_file_include_udp_bridge_udp_bridge.h:

Program Listing for File udp_bridge.h
=====================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_udp_bridge.h>` (``include/udp_bridge/udp_bridge.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_UDP_BRIDGE_H
   #define UDP_BRIDGE_UDP_BRIDGE_H
   
   #include "rclcpp_lifecycle/lifecycle_node.hpp"
   #include "rclcpp/generic_publisher.hpp"
   #include "rclcpp/generic_subscription.hpp"
   
   #include "udp_bridge_interfaces/srv/subscribe.hpp"
   #include "udp_bridge_interfaces/srv/add_remote.hpp"
   #include "udp_bridge_interfaces/srv/list_remotes.hpp"
   
   #include <netinet/in.h>
   #include "connection.h"
   #include "packet.h"
   #include "defragmenter.h"
   #include "udp_bridge/types.h"
   #include "udp_bridge/wrapped_packet.h"
   #include "udp_bridge_interfaces/msg/connection_internal.hpp"
   #include "udp_bridge_interfaces/msg/bridge_info.hpp"
   #include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"
   //#include "std_msgs/msg/int32.hpp"
   
   namespace udp_bridge
   {
   
   class RemoteNode;
   
   class UDPBridge: public rclcpp_lifecycle::LifecycleNode
   {
   public:
     using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;
   
     UDPBridge(const std::string &node_name = "udp_bridge");
   
   
     CallbackReturn on_configure(const rclcpp_lifecycle::State &);
   
     CallbackReturn on_activate(const rclcpp_lifecycle::State & state);
   
     CallbackReturn on_deactivate(const rclcpp_lifecycle::State & state);
   
     CallbackReturn on_cleanup(const rclcpp_lifecycle::State &);
   
     CallbackReturn on_shutdown(const rclcpp_lifecycle::State & state);
   
     void spin_once();
       
   private:
     void setName(const std::string &name);
   
     void callback(std::string topic_name, std::string topic_type, std::shared_ptr<rclcpp::SerializedMessage> message);
   
   
     void decode(std::vector<uint8_t> const &message, const SourceInfo& source_info);
   
     template <typename MessageType>
     MessageType deserialize(std::vector<uint8_t> const &message)
     {
       const Packet* packet = reinterpret_cast<const Packet*>(message.data());
       auto data_size = message.size()-sizeof(PacketHeader);
       rclcpp::SerializedMessage serialized_message;
       serialized_message.reserve(data_size);
       memcpy(serialized_message.get_rcl_serialized_message().buffer, packet->data, data_size);
       serialized_message.get_rcl_serialized_message().buffer_length = data_size;
       MessageType deserialized_message;
       rclcpp::Serialization<MessageType> serializer;
       serializer.deserialize_message(&serialized_message, &deserialized_message);
       return deserialized_message;
     }
       
     void decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info);
       
     void decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info);
   
     void decodeTopicStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info);
   
     void decodeSubscribeRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);
   
     void decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);
   
     void unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info);
   
     void decodeConnection(std::vector<uint8_t> const &message, const SourceInfo& source_info);
   
     void remoteSubscribe(
       const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
       std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);
   
     void remoteAdvertise(
       const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
       std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);
   
     void addRemote(
       const std::shared_ptr<udp_bridge_interfaces::srv::AddRemote::Request> request,
       std::shared_ptr<udp_bridge_interfaces::srv::AddRemote::Response> response);
   
     void listRemotes(
       const std::shared_ptr<udp_bridge_interfaces::srv::ListRemotes::Request> request,
       std::shared_ptr<udp_bridge_interfaces::srv::ListRemotes::Response> response);
   
   
     using RemoteConnectionsList = std::map<std::string, std::vector<std::string> >;
   
     template <typename MessageType>
     MessageSizeData send(MessageType const &message, const RemoteConnectionsList& remotes, bool is_overhead);
   
     template <typename MessageType>
     MessageSizeData send(MessageType const &message, const std::string& remote, bool is_overhead);
   
     RemoteConnectionsList allRemotes() const;
   
     void statsReportCallback();
   
     void bridgeInfoCallback();
   
   
     void sendBridgeInfo();
   
     void sendConnectionRequests();
   
     std::vector< std::vector<uint8_t> > fragment(const std::vector<uint8_t>& data);
   
     void cleanupSentPackets();
   
     void resendMissingPackets();
   
     void addSubscriberConnection(std::string const &source_topic, std::string const &destination_topic, uint32_t queue_size, float period, std::string remote_node, std::string connection_id);
   
     void updateLocalSubscriptions();  
   
   
   
     //void maximumPacketSizeCallback(const std_msgs::Int32::ConstPtr& msg);
   
     std::string name_;
   
     int m_socket;
     uint16_t m_port {4200};
     int m_max_packet_size {65500};
     uint32_t next_fragmented_packet_id_ {0};
   
     rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr subscribe_service_;
     rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr advertise_service_;
     rclcpp::Service<udp_bridge_interfaces::srv::AddRemote>::SharedPtr add_remote_service_;
     rclcpp::Service<udp_bridge_interfaces::srv::ListRemotes>::SharedPtr list_remotes_service_;
   
     rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::TopicStatisticsArray>::SharedPtr topic_statistics_publisher_;
     rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::BridgeInfo>::SharedPtr bridge_info_publisher_;
   
     //rclcpp::Subscriber  maximum_packet_size_subscriber_;
       
     std::map<std::string, SubscriberDetails> m_subscribers;
   
     std::map<std::string, rclcpp::GenericPublisher::SharedPtr> m_publishers;
   
     rclcpp::TimerBase::SharedPtr stats_report_timer_;
     rclcpp::TimerBase::SharedPtr bridge_info_timer_;
     rclcpp::TimerBase::SharedPtr spin_timer_;
     rclcpp::TimerBase::SharedPtr subscription_update_timer_;
   
     uint64_t next_packet_number_ = 0;
     rclcpp::Time last_packet_number_assign_time_;
   
     uint64_t next_connection_internal_message_sequence_number_ = 0;
   
     struct PendingConnection
     {
       udp_bridge_interfaces::msg::ConnectionInternal message;
       std::shared_ptr<Connection> connection;
     };
   
     std::map<uint64_t, PendingConnection> pending_connections_;
   
     std::map<std::string, std::shared_ptr<RemoteNode> > remote_nodes_;
   };
   
   } // namespace udp_bridge
   
   #endif

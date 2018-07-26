#ifndef UDP_BRIDGE_H
#define UDP_BRIDGE_H

#include <memory>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <mutex>
#include "ros/ros.h"
#include "project11/mutex_protected_bag_writer.h"

namespace udp_bridge
{
    enum Channel : uint32_t
    {
        position,
        appcast,
        active,
        wpt_updates,
        origin,
	helm_mode,
        heartbeat,
        loiter_updates,
        flir_engine,
        heading,
        ais,
        view_point,
        view_polygon,
        view_seglist,
        diagnostics,
        posmv_orientation,
        posmv_position,
        mission_plan
    };

    class UDPROSNode
    {
    public:
        UDPROSNode(std::string const &host, int send_port, int receive_port, std::string const & log_filename)
        {
            m_bag = std::shared_ptr<MutexProtectedBagWriter>(new MutexProtectedBagWriter);
            m_bag->open(log_filename, rosbag::bagmode::Write);
            
            m_socket = socket(AF_INET, SOCK_DGRAM, 0);
            if(m_socket < 0)
            {
                ROS_ERROR("Failed creating socket");
                exit(1);
            }
            
            sockaddr_in bind_address; // address to listen on
            memset((char *)&bind_address, 0, sizeof(bind_address));
            bind_address.sin_family = AF_INET;
            bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
            bind_address.sin_port = htons(receive_port);
            
            if(bind(m_socket, (sockaddr*)&bind_address, sizeof(bind_address)) < 0)
            {
                ROS_ERROR("Error binding socket");
                exit(1);
            }
            
            memset((char *)&m_send_address, 0, sizeof(m_send_address));
            m_send_address.sin_family = AF_INET;
            m_send_address.sin_port = htons(send_port);


            hostent *address_list = gethostbyname(host.c_str());
            if(!address_list)
            {
                ROS_ERROR_STREAM("Error resolving address: " << host);
                exit(1);
            }
            
            memcpy((void *)&m_send_address.sin_addr, address_list->h_addr_list[0], address_list->h_length);

            timeval socket_timeout;
            socket_timeout.tv_sec = 0;
            socket_timeout.tv_usec = 1000;
            if(setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) < 0)
            {
                ROS_ERROR("Error setting socket timeout");
                exit(1);
            }
            
            int broadcast_enabled = 1;
            if(setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, &broadcast_enabled, sizeof(broadcast_enabled)) < 0)
            {
                ROS_ERROR("Error enabling socket broadcast");
                exit(1);
            }

        }
        
        ~UDPROSNode()
        {
            m_bag->close();
        }

        template<typename ROS_TYPE, Channel C> void addSender(std::string const &topic)
        {
            m_ros_subscribers[C] = m_nodeHandle.subscribe(topic,10, &UDPROSNode::ROSCallback<ROS_TYPE, C>, this);
            topic_map[C] = topic;
        }

        template<typename ROS_TYPE, Channel C> void addReceiver(std::string const &topic, bool latch = false)
        {
            m_ros_publishers[C].rpub = m_nodeHandle.advertise<ROS_TYPE>(topic,10, latch);
            m_ros_publishers[C].decoder = &UDPROSNode::Decode<ROS_TYPE>;
            topic_map[C] = topic;
        }
        
        void spin()
        {
            while(ros::ok())
            {   
                sockaddr_in remote_address;
                socklen_t remote_address_length = sizeof(remote_address);
                int receive_length = 0;
                std::vector<uint8_t> buffer;
                {
                    std::lock_guard<std::mutex> lock(m_socket_mutex);
                    int buffer_size;
                    unsigned int buffer_size_size = sizeof(buffer_size);
                    getsockopt(m_socket,SOL_SOCKET,SO_RCVBUF,&buffer_size,&buffer_size_size);
                    buffer.resize(buffer_size);
                    receive_length = recvfrom(m_socket, &buffer.front(), buffer_size, 0, (sockaddr*)&remote_address, &remote_address_length);
                    
                }
                if(receive_length > 0)
                {
                    //std::cerr << receive_length << std::endl;
                    buffer.resize(receive_length);
                    Channel c = *static_cast<Channel*>(reinterpret_cast<void*>(buffer.data()));
                    if(m_ros_publishers.find(c) != m_ros_publishers.end())
                        m_ros_publishers[c].decoder(buffer,m_ros_publishers[c].rpub,m_bag,c);
                }

                ros::spinOnce();
                //ros::Duration(0.001).sleep();
            }
        }
        

        
    private:
        int m_socket;
        std::mutex m_socket_mutex;
        sockaddr_in m_send_address;
        ros::NodeHandle m_nodeHandle;
        
        std::shared_ptr<MutexProtectedBagWriter> m_bag;
        
        static std::map<Channel,std::string> topic_map;
        
        template<typename ROS_TYPE, Channel C> void ROSCallback(const typename ROS_TYPE::ConstPtr& inmsg)
        {
            uint32_t channel = C;

            uint32_t serial_size = ros::serialization::serializationLength(*inmsg);
            boost::shared_array<uint8_t> buffer(new uint8_t[serial_size]);
            ros::serialization::OStream stream(buffer.get(), serial_size);
            ros::serialization::serialize(stream,*inmsg);
 
            std::vector<uint8_t> send_buffer(sizeof(channel)+serial_size);
            //std::cerr << "send buffer size: " << send_buffer.size() << std::endl;
            
            memcpy(send_buffer.data(),&channel,sizeof(channel));
            memcpy(&(send_buffer.data()[sizeof(channel)]),buffer.get(),serial_size);
            
            {
                std::lock_guard<std::mutex> lock(m_socket_mutex);
                if(sendto(m_socket, send_buffer.data(), send_buffer.size(), 0, (sockaddr*)&m_send_address, sizeof(m_send_address)) < 0)
                {
                    ROS_ERROR("sento fail");
                }
            }
            if(m_bag && ros::Time::now() > ros::TIME_MIN)
                m_bag->write(topic_map[C],ros::Time::now(),*inmsg);
        }

        typedef std::map<Channel,ros::Subscriber> SubscriberMap;
        SubscriberMap m_ros_subscribers;

        template<typename ROS_TYPE> static void Decode(std::vector<uint8_t> &message, ros::Publisher &pub, std::shared_ptr<MutexProtectedBagWriter> bag, Channel channel)
        {
            ROS_TYPE ros_msg;
            boost::shared_array<uint8_t> buffer(new uint8_t[message.size()-sizeof(uint32_t)]);
            memcpy(buffer.get(),&(message.data()[sizeof(uint32_t)]),message.size()-sizeof(uint32_t));
            
            ros::serialization::IStream stream(buffer.get(),message.size());
            ros::serialization::Serializer<ROS_TYPE>::read(stream, ros_msg);
            
            pub.publish(ros_msg);
            
            if(bag && ros::Time::now() > ros::TIME_MIN)
                bag->write(topic_map[channel],ros::Time::now(),ros_msg);
        }
        
        struct ROSPublisher
        {
            ros::Publisher rpub;
            void (*decoder)(std::vector<uint8_t> &, ros::Publisher &, std::shared_ptr<MutexProtectedBagWriter>, Channel);
        };
        
        typedef std::map<Channel,ROSPublisher> PublisherMap;

        PublisherMap m_ros_publishers;

    };
    
}

#endif

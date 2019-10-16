#include "udp_bridge/udp_bridge.h"
#include <sensor_msgs/CompressedImage.h>

std::map<udp_bridge::Channel,std::string> udp_bridge::UDPROSNode::topic_map;

int main(int argc, char **argv)
{
    ros::init(argc, argv, "udp_bridge_johnny5_client");

    std::string host = "localhost";
    if (argc > 1)
        host = argv[1];

    int send_port = 4205;
    int receive_port = 4204;
    
    udp_bridge::UDPROSNode n(host,send_port,receive_port);
    
    n.addReceiver<sensor_msgs::CompressedImage,udp_bridge::johnny5_images>("/udp/camera_johnny5/image_raw/compressed");
    
    n.spin();

    return 0;
}

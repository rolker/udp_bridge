#include "udp_bridge/udp_bridge.h"
#include "geographic_msgs/GeoPointStamped.h"

std::map<udp_bridge::Channel,std::string> udp_bridge::UDPROSNode::topic_map;

int main(int argc, char **argv)
{
    ros::init(argc, argv, "udp_position_server");

    std::string host = "localhost";
    if (argc > 1)
        host = argv[1];

    int send_port = 4204;
    int receive_port = 4205;
    
    udp_bridge::UDPROSNode n(host,send_port,receive_port);

    n.addSender<geographic_msgs::GeoPointStamped,udp_bridge::position>("/udp/generated_position");

    n.spin();

    return 0;
}

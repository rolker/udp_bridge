#include "udp_bridge/udp_bridge.h"

int main(int argc, char **argv)
{
    ros::init(argc, argv, "udp_bridge_node");

    udp_bridge::UDPBridge bridge;
    
    bridge.spin();
    return 0;
}


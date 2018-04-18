#include "udp_bridge/udp_bridge.h"
#include "std_msgs/String.h"
#include "std_msgs/Bool.h"
#include "geographic_msgs/GeoPointStamped.h"
#include "asv_msgs/VehicleStatus.h"
#include "asv_msgs/AISContact.h"
#include "mission_plan/NavEulerStamped.h"
#include <regex>
#include "boost/date_time/posix_time/posix_time.hpp"

std::map<udp_bridge::Channel,std::string> udp_bridge::UDPROSNode::topic_map;

int main(int argc, char **argv)
{
    ros::init(argc, argv, "udp_bridge_operator");

    std::string host = "localhost";
    if (argc > 1)
        host = argv[1];


    boost::posix_time::ptime now = ros::WallTime::now().toBoost();
    std::string iso_now = std::regex_replace(boost::posix_time::to_iso_extended_string(now),std::regex(":"),"-");
    
    std::string log_filename = "nodes/udp_bridge_operator-"+iso_now+".bag";
    
    int send_port = 4201;
    int receive_port = 4200;
    
    udp_bridge::UDPROSNode n(host,send_port,receive_port,log_filename);
    
    n.addSender<std_msgs::Bool, udp_bridge::active>("/udp/active");
    n.addSender<std_msgs::String, udp_bridge::helm_mode>("/udp/helm_mode");
    n.addSender<std_msgs::String, udp_bridge::wpt_updates>("/udp/wpt_updates");
    n.addSender<std_msgs::String, udp_bridge::loiter_updates>("/udp/loiter_updates");

    n.addReceiver<geographic_msgs::GeoPointStamped,udp_bridge::position>("/udp/position");
    n.addReceiver<std_msgs::String,udp_bridge::appcast>("/udp/appcast");
    n.addReceiver<geographic_msgs::GeoPoint,udp_bridge::origin>("/udp/origin");
    n.addReceiver<asv_msgs::VehicleStatus,udp_bridge::vehicle_status>("/udp/vehicle_status");
    n.addReceiver<std_msgs::String, udp_bridge::flir_engine>("/udp/flir_engine");
    n.addReceiver<mission_plan::NavEulerStamped, udp_bridge::heading>("/udp/heading");
    n.addReceiver<asv_msgs::AISContact, udp_bridge::ais>("/udp/ais");
    
    n.spin();

    return 0;
}

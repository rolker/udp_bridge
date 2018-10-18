#include "udp_bridge/udp_bridge.h"
#include "std_msgs/String.h"
#include "std_msgs/Bool.h"
#include "geographic_msgs/GeoPointStamped.h"
#include "marine_msgs/Heartbeat.h"
#include "marine_msgs/Contact.h"
#include "diagnostic_msgs/DiagnosticArray.h"
#include "marine_msgs/NavEulerStamped.h"
#include "sensor_msgs/NavSatFix.h"
#include "geometry_msgs/TwistStamped.h"
#include "geographic_msgs/GeoPath.h"
#include <regex>
#include "boost/date_time/posix_time/posix_time.hpp"

std::map<udp_bridge::Channel,std::string> udp_bridge::UDPROSNode::topic_map;

int main(int argc, char **argv)
{
    ros::init(argc, argv, "udp_bridge_vehicle");
    
    std::string host = "localhost";
    if (argc > 1)
        host = argv[1];
  
    
    boost::posix_time::ptime now = ros::WallTime::now().toBoost();
    std::string iso_now = std::regex_replace(boost::posix_time::to_iso_extended_string(now),std::regex(":"),"-");
    
    std::string log_filename = "nodes/udp_bridge_vehicle-"+iso_now+".bag";
    
    int send_port = 4200;
    int receive_port = 4201;
    
    udp_bridge::UDPROSNode n(host,send_port,receive_port,log_filename);

    n.addSender<geographic_msgs::GeoPointStamped, udp_bridge::position>("/position");
    n.addSender<std_msgs::String, udp_bridge::appcast>("/moos/appcast");
    n.addSender<geographic_msgs::GeoPoint,udp_bridge::origin>("/origin");
    n.addSender<marine_msgs::Heartbeat,udp_bridge::heartbeat>("/heartbeat");
    n.addSender<std_msgs::String, udp_bridge::flir_engine>("/flir_engine");
    n.addSender<marine_msgs::NavEulerStamped, udp_bridge::heading>("/heading");
    n.addSender<marine_msgs::Contact, udp_bridge::contact>("/contact");
    n.addSender<std_msgs::String, udp_bridge::view_point, true>("/moos/view_point");
    n.addSender<std_msgs::String, udp_bridge::view_polygon, true>("/moos/view_polygon");
    n.addSender<std_msgs::String, udp_bridge::view_seglist, true>("/moos/view_seglist");
    n.addSender<diagnostic_msgs::DiagnosticArray, udp_bridge::diagnostics>("/diagnostics");
    n.addSender<marine_msgs::NavEulerStamped, udp_bridge::posmv_orientation>("/posmv/orientation");
    n.addSender<sensor_msgs::NavSatFix, udp_bridge::posmv_position>("/posmv/position");
    n.addSender<geometry_msgs::TwistStamped, udp_bridge::sog>("/sog");
    n.addSender<geographic_msgs::GeoPath, udp_bridge::coverage>("/coverage");
    n.addSender<geographic_msgs::GeoPath, udp_bridge::mbes_ping>("/mbes_ping");
    
    n.addReceiver<std_msgs::Bool,udp_bridge::active>("/active");
    n.addReceiver<std_msgs::String,udp_bridge::helm_mode>("/helm_mode");
    n.addReceiver<std_msgs::String,udp_bridge::wpt_updates>("/moos/wpt_updates");
    n.addReceiver<std_msgs::String,udp_bridge::loiter_updates>("/moos/loiter_updates");
    n.addReceiver<std_msgs::String,udp_bridge::mission_plan>("/mission_plan");

    n.spin();
    
    return 0;
}

